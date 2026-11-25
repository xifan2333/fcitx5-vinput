#include "daemon/asr/hotword_utils.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fast_float/fast_float.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <utility>
#include <vector>

#include "common/asr/bpe_vocab_exporter.h"
#include "common/asr/model_manager.h"
#include "common/utils/file_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/sherpa_json_helpers.h"

namespace vinput::daemon::asr {

namespace {

constexpr std::uintmax_t kMaxHotwordFileBytes = 1024ULL * 1024ULL;

bool SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

struct ScorePrefix {
  std::string_view digits;
  bool is_hex = false;
  bool is_negative = false;
};

std::optional<ScorePrefix> StripScorePrefix(std::string_view value) noexcept {
  if (value.empty()) {
    return std::nullopt;
  }
  std::string_view digits = value;
  bool is_negative = false;
  if (digits.starts_with('+') || digits.starts_with('-')) {
    is_negative = digits.starts_with('-');
    digits.remove_prefix(1);
    if (digits.empty() || digits.starts_with('+') || digits.starts_with('-')) {
      return std::nullopt;
    }
  }
  bool is_hex = false;
  if (digits.starts_with("0x") || digits.starts_with("0X")) {
    digits.remove_prefix(2);
    is_hex = true;
  }
  return ScorePrefix{.digits = digits, .is_hex = is_hex, .is_negative = is_negative};
}

int HexDigitValue(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

std::optional<int> ParseHexExponent(std::string_view suffix) noexcept {
  if (suffix.empty()) {
    return std::nullopt;
  }
  if (suffix.front() == '+') {
    suffix.remove_prefix(1);
    if (suffix.empty() || suffix.front() == '+' || suffix.front() == '-') {
      return std::nullopt;
    }
  }
  int exp = 0;
  const auto [ptr, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), exp);
  if (ec != std::errc{} || ptr != suffix.data() + suffix.size()) {
    return std::nullopt;
  }
  return exp;
}

struct HexMantissa {
  std::uint64_t significand = 0;
  int hex_digits = 0;
  long long exp_offset = 0;
};

void AccumulateHexDigit(HexMantissa& state, int nibble, bool seen_dot) noexcept {
  constexpr int kMaxKeptHexDigits = 15;
  if (state.significand == 0) {
    if (nibble == 0) {
      if (seen_dot) {
        state.exp_offset -= 4;
      }
      return;
    }
    state.significand = static_cast<std::uint64_t>(nibble);
    state.hex_digits = 1;
    if (seen_dot) {
      state.exp_offset -= 4;
    }
    return;
  }
  if (state.hex_digits < kMaxKeptHexDigits) {
    state.significand = (state.significand << 4) | static_cast<std::uint64_t>(nibble);
    ++state.hex_digits;
    if (seen_dot) {
      state.exp_offset -= 4;
    }
    return;
  }
  if (!seen_dot) {
    state.exp_offset += 4;
  }
  if (nibble != 0) {
    state.significand |= 1ULL;
  }
}

std::optional<HexMantissa> ParseHexMantissa(std::string_view mantissa_str) noexcept {
  HexMantissa state;
  bool seen_dot = false;
  bool seen_digit = false;
  for (const char ch : mantissa_str) {
    if (ch == '.') {
      if (seen_dot) {
        return std::nullopt;
      }
      seen_dot = true;
      continue;
    }
    const int nibble = HexDigitValue(ch);
    if (nibble < 0) {
      return std::nullopt;
    }
    seen_digit = true;
    AccumulateHexDigit(state, nibble, seen_dot);
  }
  if (!seen_digit) {
    return std::nullopt;
  }
  return state;
}

std::optional<float> ParseHexFloat(std::string_view digits) noexcept {
  if (digits.empty()) {
    return std::nullopt;
  }
  const auto exp_pos = digits.find_first_of("pP");
  const std::string_view mantissa_str = digits.substr(0, exp_pos);
  int exp = 0;
  if (exp_pos != std::string_view::npos) {
    const auto parsed_exp = ParseHexExponent(digits.substr(exp_pos + 1));
    if (!parsed_exp) {
      return std::nullopt;
    }
    exp = *parsed_exp;
  }

  const auto mantissa = ParseHexMantissa(mantissa_str);
  if (!mantissa) {
    return std::nullopt;
  }
  if (mantissa->significand == 0) {
    return 0.0F;
  }

  const int msb_bit = 63 - std::countl_zero(mantissa->significand);
  const long long total_exp = static_cast<long long>(exp) + mantissa->exp_offset;
  const long long norm_exp = total_exp + static_cast<long long>(msb_bit);
  if (norm_exp >= 128) {
    return std::nullopt;
  }
  if (norm_exp < -151) {
    return 0.0F;
  }
  const auto result = static_cast<float>(
      std::ldexp(static_cast<double>(mantissa->significand), static_cast<int>(total_exp)));
  if (!std::isfinite(result)) {
    return std::nullopt;
  }
  return result;
}

// Hotword scores are passed through to sherpa-onnx, so only plain finite
// numbers are accepted (no NaN/Inf, no locale-dependent parsing).
std::optional<float> ParseHotwordScore(std::string_view value) noexcept {
  const auto prefix = StripScorePrefix(value);
  if (!prefix) {
    return std::nullopt;
  }
  float parsed = 0.0F;
  if (prefix->is_hex) {
    const auto hex_val = ParseHexFloat(prefix->digits);
    if (!hex_val) {
      return std::nullopt;
    }
    parsed = *hex_val;
  } else {
    const auto [end, ec] = fast_float::from_chars(
        prefix->digits.data(), prefix->digits.data() + prefix->digits.size(), parsed);
    if (ec != std::errc{} || end != prefix->digits.data() + prefix->digits.size() ||
        !std::isfinite(parsed)) {
      return std::nullopt;
    }
  }
  return prefix->is_negative ? -parsed : parsed;
}

struct Utf8Validation {
  bool valid = true;
  size_t error_offset = 0;
  const char* error_reason = nullptr;
};

// Hotword files must be well-formed UTF-8 without embedded NULs or control
// characters, because the text is forwarded verbatim to the ASR engine.
Utf8Validation ValidateHotwordText(std::string_view text) noexcept {
  size_t offset = 0;
  while (offset < text.size()) {
    size_t bytes = 0;
    char32_t cp = 0;
    if (!unicode::utf8::decode_codepoint(text.data() + offset, text.size() - offset, bytes, cp)) {
      return {.valid = false, .error_offset = offset, .error_reason = "invalid UTF-8 sequence"};
    }
    if (cp == 0) {
      return {
          .valid = false, .error_offset = offset, .error_reason = "embedded NUL is not allowed"};
    }
    const bool is_ascii_whitespace = (cp == '\t' || cp == '\n' || cp == '\r');
    if (!is_ascii_whitespace &&
        (unicode::general_category(cp) == unicode::GeneralCategory::Cc || cp < 0x20)) {
      return {.valid = false,
              .error_offset = offset,
              .error_reason = "control character is not allowed"};
    }
    offset += bytes;
  }
  return {.valid = true, .error_offset = 0, .error_reason = nullptr};
}

constexpr bool IsHotwordWhitespace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool CheckHotwordFileMetadata(const std::filesystem::path& file_path, std::string* error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file_path, ec) || ec) {
    return SetError(error, "hotwords file is not a regular file: " + file_path.string());
  }
  const auto size = std::filesystem::file_size(file_path, ec);
  if (ec) {
    return SetError(error, "failed to inspect hotwords file '" + file_path.string() +
                               "': " + ec.message());
  }
  if (size > kMaxHotwordFileBytes) {
    return SetError(error, "hotwords file exceeds 1 MiB limit: " + file_path.string());
  }
  return true;
}

bool ValidateLoadedHotwordContent(const std::string& text, const std::string& path,
                                  std::string* error) {
  if (text.size() > kMaxHotwordFileBytes) {
    return SetError(error, "hotwords file exceeds 1 MiB limit: " + path);
  }
  const auto utf8_res = ValidateHotwordText(text);
  if (!utf8_res.valid) {
    return SetError(error, "invalid hotwords file at byte " +
                               std::to_string(utf8_res.error_offset) + ": " +
                               utf8_res.error_reason);
  }
  return true;
}

bool ReadHotwordFile(const std::string& path, std::string* content, std::string* error) {
  if (path.empty()) {
    if (content != nullptr) {
      content->clear();
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const std::filesystem::path file_path(path);
  if (!CheckHotwordFileMetadata(file_path, error)) {
    return false;
  }

  auto read_res = vinput::file::ReadTextFile(file_path, kMaxHotwordFileBytes + 1);
  if (!read_res) {
    return SetError(error, "failed to read hotwords file: " + path);
  }

  if (!ValidateLoadedHotwordContent(*read_res, path, error)) {
    return false;
  }

  if (content != nullptr) {
    *content = std::move(*read_res);
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

struct HotwordEntry {
  std::string text;
  std::string score;
};

bool IsValidScore(std::string_view value) {
  return ParseHotwordScore(value).has_value();
}

bool LooksLikeScore(std::string_view value) {
  if (value.empty()) {
    return true;
  }
  const auto first = static_cast<unsigned char>(value.front());
  if ((first >= '0' && first <= '9') || first == '+' || first == '-' || first == '.') {
    return true;
  }

  return vinput::str::EqualsIgnoreCase(value, "nan") ||
         vinput::str::StartsWithIgnoreCase(value, "nan(") ||
         vinput::str::EqualsIgnoreCase(value, "inf") ||
         vinput::str::EqualsIgnoreCase(value, "infinity");
}

bool HasSpaceBeforeAnyColon(std::string_view line) noexcept {
  for (std::size_t i = 1; i < line.size(); ++i) {
    if (line[i] == ':' && IsHotwordWhitespace(line[i - 1])) {
      return true;
    }
  }
  return false;
}

bool ParseColonHotwordEntry(std::string_view raw_text, std::string_view raw_score,
                            std::string_view line, HotwordEntry* entry, std::string* error) {
  const std::string_view score = vinput::str::Trim(raw_score);
  const std::string_view text = vinput::str::Trim(raw_text);
  const bool has_whitespace =
      (!raw_text.empty() && IsHotwordWhitespace(raw_text.back())) || (raw_score != score);

  if (has_whitespace) {
    return SetError(error,
                    "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line));
  }

  if (IsValidScore(score)) {
    if (text.empty()) {
      return SetError(error, "hotword score has no preceding word or phrase: " + std::string(line));
    }
    entry->text = std::string(text);
    entry->score = std::string(score);
    return true;
  }

  if (LooksLikeScore(score)) {
    return SetError(error,
                    "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line));
  }

  entry->text = std::string(line);
  entry->score.clear();
  return true;
}

bool ParseHotwordEntry(std::string_view raw_line, HotwordEntry* entry, std::string* error) {
  const std::string_view line = vinput::str::Trim(raw_line);
  if (line.empty()) {
    entry->text.clear();
    entry->score.clear();
    return true;
  }

  if (HasSpaceBeforeAnyColon(line)) {
    return SetError(error,
                    "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line));
  }

  const auto pair = vinput::str::RSplitOnce(line, ':');
  if (!pair) {
    entry->text = std::string(line);
    entry->score.clear();
    return true;
  }

  return ParseColonHotwordEntry(pair->first, pair->second, line, entry, error);
}

bool LoadHotwordEntries(const std::string& path, std::vector<HotwordEntry>* entries,
                        std::string* error) {
  std::string content;
  if (!ReadHotwordFile(path, &content, error)) {
    return false;
  }

  entries->clear();
  const auto lines = vinput::str::SplitLines(content);
  entries->reserve(lines.size());
  for (const std::string_view line : lines) {
    HotwordEntry entry;
    if (!ParseHotwordEntry(line, &entry, error)) {
      return false;
    }
    if (!entry.text.empty()) {
      entries->push_back(std::move(entry));
    }
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

constexpr bool IsSupportedTransducerUnit(std::string_view unit) noexcept {
  return unit == "cjkchar" || unit == "bpe" || unit == "bbpe" || unit == "cjkchar+bpe";
}

constexpr bool RequiresBpeVocabulary(std::string_view unit) noexcept {
  return unit == "bpe" || unit == "bbpe" || unit == "cjkchar+bpe";
}

bool ValidateTransducerBpeAsset(const ModelInfo& info, std::string_view modeling_unit,
                                std::string* error) {
  const std::string bpe_vocab = info.File("bpe_vocab");
  if (bpe_vocab.empty() || !std::filesystem::is_regular_file(bpe_vocab)) {
    return SetError(error,
                    "modeling_unit '" + std::string(modeling_unit) + "' requires model.bpe_vocab");
  }
  auto valid_res = ValidateBpeVocabulary(bpe_vocab);
  if (!valid_res) {
    return SetError(error, valid_res.error());
  }
  return true;
}

} // namespace

bool IsTransducerHotwordFamily(std::string_view family) {
  return family == "transducer" || family == "nemo_transducer";
}

bool IsPromptHotwordFamily(std::string_view family) {
  return family == "funasr_nano" || family == "qwen3_asr";
}

bool LoadTransducerHotwords(const std::string& path, std::string* hotwords, std::string* error) {
  std::vector<HotwordEntry> entries;
  if (!LoadHotwordEntries(path, &entries, error)) {
    return false;
  }

  size_t total_size = 0;
  for (const auto& entry : entries) {
    total_size += entry.text.size() + 1;
    if (!entry.score.empty()) {
      total_size += 2 + entry.score.size();
    }
  }

  hotwords->clear();
  hotwords->reserve(total_size);
  for (const auto& entry : entries) {
    *hotwords += entry.text;
    if (!entry.score.empty()) {
      // sherpa-onnx expects the score as a separate trailing token.
      *hotwords += " :" + entry.score;
    }
    hotwords->push_back('\n');
  }
  return true;
}

bool LoadPromptHotwordsCsv(const std::string& path, std::string* hotwords, std::string* error) {
  std::vector<HotwordEntry> entries;
  if (!LoadHotwordEntries(path, &entries, error)) {
    return false;
  }

  std::vector<std::string_view> words;
  words.reserve(entries.size());
  for (const auto& entry : entries) {
    words.emplace_back(entry.text);
  }
  *hotwords = vinput::str::Join(words, ",");
  return true;
}

bool ValidateTransducerHotwordAssets(const ModelInfo& info, std::string* error) {
  const std::string modeling_unit = JsonString(info.model_config, "modeling_unit");
  if (modeling_unit.empty()) {
    return SetError(error, "hotword-capable transducer is missing model.modeling_unit; "
                           "reinstall or update the model");
  }
  if (!IsSupportedTransducerUnit(modeling_unit)) {
    return SetError(error, "unsupported sherpa-onnx hotword modeling_unit '" + modeling_unit + "'");
  }

  if (RequiresBpeVocabulary(modeling_unit) &&
      !ValidateTransducerBpeAsset(info, modeling_unit, error)) {
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace vinput::daemon::asr
