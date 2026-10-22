#include "daemon/asr/hotword_utils.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Hotword scores are passed through to sherpa-onnx, so only plain finite
// decimals are accepted (no NaN/Inf, no locale-dependent parsing).
bool ParseHotwordScore(std::string_view value, float* out = nullptr) noexcept {
  if (value.empty()) {
    return false;
  }
  std::string_view digits = value;
  bool is_negative = false;
  if (digits.starts_with('+')) {
    digits.remove_prefix(1);
    if (digits.empty() || digits.starts_with('+') || digits.starts_with('-')) {
      return false;
    }
  } else if (digits.starts_with('-')) {
    is_negative = true;
    digits.remove_prefix(1);
    if (digits.empty() || digits.starts_with('+') || digits.starts_with('-')) {
      return false;
    }
  }
  std::chars_format format = std::chars_format::general;
  if (digits.starts_with("0x") || digits.starts_with("0X")) {
    digits.remove_prefix(2);
    format = std::chars_format::hex;
  }
  float parsed = 0.0F;
  const auto [end, ec] =
      std::from_chars(digits.data(), digits.data() + digits.size(), parsed, format);
  if (ec != std::errc{} || end != digits.data() + digits.size() || !std::isfinite(parsed)) {
    return false;
  }
  if (is_negative) {
    parsed = -parsed;
  }
  if (out != nullptr) {
    *out = parsed;
  }
  return true;
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

  std::error_code ec;
  const std::filesystem::path file_path(path);
  if (!std::filesystem::is_regular_file(file_path, ec) || ec) {
    if (error != nullptr) {
      *error = "hotwords file is not a regular file: " + path;
    }
    return false;
  }
  const auto size = std::filesystem::file_size(file_path, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to inspect hotwords file '" + path + "': " + ec.message();
    }
    return false;
  }
  if (size > kMaxHotwordFileBytes) {
    if (error != nullptr) {
      *error = "hotwords file exceeds 1 MiB limit: " + path;
    }
    return false;
  }

  auto read_res = vinput::file::ReadTextFile(file_path, kMaxHotwordFileBytes + 1);
  if (!read_res) {
    if (error != nullptr) {
      *error = "failed to read hotwords file: " + path;
    }
    return false;
  }
  std::string text = std::move(*read_res);
  if (text.size() > kMaxHotwordFileBytes) {
    if (error != nullptr) {
      *error = "hotwords file exceeds 1 MiB limit: " + path;
    }
    return false;
  }
  const auto utf8_res = ValidateHotwordText(text);
  if (!utf8_res.valid) {
    if (error != nullptr) {
      *error = "invalid hotwords file at byte " + std::to_string(utf8_res.error_offset) + ": " +
               utf8_res.error_reason;
    }
    return false;
  }
  if (content != nullptr) {
    *content = std::move(text);
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
  return ParseHotwordScore(value);
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

bool ParseHotwordEntry(std::string_view raw_line, HotwordEntry* entry, std::string* error) {
  const std::string_view line = vinput::str::Trim(raw_line);
  if (line.empty()) {
    entry->text.clear();
    entry->score.clear();
    return true;
  }

  for (std::size_t i = 1; i < line.size(); ++i) {
    if (line[i] == ':' && IsHotwordWhitespace(line[i - 1])) {
      if (error != nullptr) {
        *error = "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line);
      }
      return false;
    }
  }

  const auto pair = vinput::str::RSplitOnce(line, ':');
  if (!pair) {
    entry->text = std::string(line);
    entry->score.clear();
    return true;
  }

  const std::string_view raw_text = pair->first;
  const std::string_view raw_score = pair->second;
  const std::string_view score = vinput::str::Trim(raw_score);
  const std::string_view text = vinput::str::Trim(raw_text);
  if (IsValidScore(score)) {
    const bool whitespace_before = !raw_text.empty() && IsHotwordWhitespace(raw_text.back());
    const bool whitespace_after = raw_score != score;
    if (whitespace_before || whitespace_after) {
      if (error != nullptr) {
        *error = "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line);
      }
      return false;
    }
    if (text.empty()) {
      if (error != nullptr) {
        *error = "hotword score has no preceding word or phrase: " + std::string(line);
      }
      return false;
    }
    entry->text = std::string(text);
    entry->score = std::string(score);
    return true;
  }

  // A separated ':...' token or an attached numeric-looking suffix is an
  // invalid score. Other colon-containing terms (for example URLs) remain
  // literal text.
  const bool whitespace_before = !raw_text.empty() && IsHotwordWhitespace(raw_text.back());
  const bool whitespace_after = raw_score != score;
  if (whitespace_before || whitespace_after || LooksLikeScore(score)) {
    if (error != nullptr) {
      *error = "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line);
    }
    return false;
  }

  entry->text = std::string(line);
  entry->score.clear();
  return true;
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
    if (error != nullptr) {
      *error = "hotword-capable transducer is missing model.modeling_unit; "
               "reinstall or update the model";
    }
    return false;
  }
  if (modeling_unit != "cjkchar" && modeling_unit != "bpe" && modeling_unit != "bbpe" &&
      modeling_unit != "cjkchar+bpe") {
    if (error != nullptr) {
      *error = "unsupported sherpa-onnx hotword modeling_unit '" + modeling_unit + "'";
    }
    return false;
  }

  if (modeling_unit == "bpe" || modeling_unit == "bbpe" || modeling_unit == "cjkchar+bpe") {
    const std::string bpe_vocab = info.File("bpe_vocab");
    if (bpe_vocab.empty() || !std::filesystem::is_regular_file(bpe_vocab)) {
      if (error != nullptr) {
        *error = "modeling_unit '" + modeling_unit + "' requires model.bpe_vocab";
      }
      return false;
    }
    if (!ValidateBpeVocabulary(bpe_vocab, error)) {
      return false;
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace vinput::daemon::asr
