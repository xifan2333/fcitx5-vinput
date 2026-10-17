#include "daemon/asr/hotword_utils.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <vector>

#include "common/asr/bpe_vocab_exporter.h"
#include "common/asr/model_manager.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/sherpa_json_helpers.h"

namespace vinput::daemon::asr {

namespace {

constexpr std::uintmax_t kMaxHotwordFileBytes = 1024ULL * 1024ULL;

bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool ValidateHotwordUtf8(std::string_view text, std::string* error) {
  auto fail = [&](std::size_t offset, std::string_view reason) {
    if (error) {
      *error =
          "invalid hotwords file at byte " + std::to_string(offset) + ": " + std::string(reason);
    }
    return false;
  };

  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[offset]);
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (first <= 0x7f) {
      length = 1;
      codepoint = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      codepoint = first & 0x07;
    } else {
      return fail(offset, "invalid UTF-8 leading byte");
    }

    if (length > text.size() - offset) {
      return fail(offset, "truncated UTF-8 sequence");
    }
    for (std::size_t i = 1; i < length; ++i) {
      const auto continuation = static_cast<unsigned char>(text[offset + i]);
      if ((continuation & 0xc0) != 0x80) {
        return fail(offset + i, "invalid UTF-8 continuation byte");
      }
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }

    const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                          (length == 4 && codepoint < 0x10000);
    if (overlong) {
      return fail(offset, "overlong UTF-8 sequence");
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
      return fail(offset, "UTF-8 encodes a surrogate code point");
    }
    if (codepoint > 0x10ffff) {
      return fail(offset, "UTF-8 code point exceeds U+10FFFF");
    }
    if (codepoint == 0) {
      return fail(offset, "embedded NUL is not allowed");
    }
    const bool permitted_ascii_whitespace =
        codepoint == '\t' || codepoint == '\n' || codepoint == '\r';
    if ((!permitted_ascii_whitespace && codepoint < 0x20) ||
        (codepoint >= 0x7f && codepoint <= 0x9f)) {
      return fail(offset, "control character is not allowed");
    }
    offset += length;
  }
  return true;
}

bool ReadHotwordFile(const std::string& path, std::string* content, std::string* error) {
  if (path.empty()) {
    if (content) {
      content->clear();
    }
    if (error) {
      error->clear();
    }
    return true;
  }

  std::error_code ec;
  const std::filesystem::path file_path(path);
  if (!std::filesystem::is_regular_file(file_path, ec) || ec) {
    if (error) {
      *error = "hotwords file is not a regular file: " + path;
    }
    return false;
  }
  const auto size = std::filesystem::file_size(file_path, ec);
  if (ec) {
    if (error) {
      *error = "failed to inspect hotwords file '" + path + "': " + ec.message();
    }
    return false;
  }
  if (size > kMaxHotwordFileBytes) {
    if (error) {
      *error = "hotwords file exceeds 1 MiB limit: " + path;
    }
    return false;
  }

  std::ifstream input(file_path, std::ios::binary);
  if (!input) {
    if (error) {
      *error = "failed to open hotwords file: " + path;
    }
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) {
    if (error) {
      *error = "failed to read hotwords file: " + path;
    }
    return false;
  }

  std::string text = std::move(buffer).str();
  if (!ValidateHotwordUtf8(text, error)) {
    return false;
  }
  if (content) {
    *content = std::move(text);
  }
  if (error) {
    error->clear();
  }
  return true;
}

struct HotwordEntry {
  std::string text;
  std::string score;
};

bool IsValidScore(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  std::istringstream input{std::string(value)};
  input.imbue(std::locale::classic());
  input >> std::noskipws;
  float score = 0.0f;
  input >> score;
  return input.eof() && !input.fail() && std::isfinite(score);
}

bool LooksLikeScore(std::string_view value) {
  if (value.empty()) {
    return true;
  }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if ((first >= '0' && first <= '9') || first == '+' || first == '-' || first == '.') {
    return true;
  }

  std::string lower(value);
  for (char& c : lower) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 'A' && byte <= 'Z') {
      c = static_cast<char>(byte - 'A' + 'a');
    }
  }
  return lower == "nan" || lower.starts_with("nan(") || lower == "inf" || lower == "infinity";
}

bool ParseHotwordEntry(std::string line, HotwordEntry* entry, std::string* error) {
  line = vinput::str::TrimAsciiWhitespace(line);
  if (line.empty()) {
    entry->text.clear();
    entry->score.clear();
    return true;
  }

  for (std::size_t i = 1; i < line.size(); ++i) {
    if (line[i] == ':' && IsAsciiWhitespace(line[i - 1])) {
      if (error) {
        *error = "invalid hotword score; use 'word:3.5' without spaces: " + line;
      }
      return false;
    }
  }

  const auto pair = vinput::str::SplitPairRight(line, ':');
  if (!pair) {
    entry->text = std::move(line);
    entry->score.clear();
    return true;
  }

  const std::string_view raw_text = pair->first;
  const std::string_view raw_score = pair->second;
  const std::string score = vinput::str::TrimAsciiWhitespace(raw_score);
  const std::string text = vinput::str::TrimAsciiWhitespace(raw_text);
  if (IsValidScore(score)) {
    const bool whitespace_before = !raw_text.empty() && IsAsciiWhitespace(raw_text.back());
    const bool whitespace_after = raw_score != score;
    if (whitespace_before || whitespace_after) {
      if (error) {
        *error = "invalid hotword score; use 'word:3.5' without spaces: " + line;
      }
      return false;
    }
    if (text.empty()) {
      if (error) {
        *error = "hotword score has no preceding word or phrase: " + line;
      }
      return false;
    }
    entry->text = text;
    entry->score = score;
    return true;
  }

  // A separated ':...' token or an attached numeric-looking suffix is an
  // invalid score. Other colon-containing terms (for example URLs) remain
  // literal text.
  const bool whitespace_before = !raw_text.empty() && IsAsciiWhitespace(raw_text.back());
  const bool whitespace_after = raw_score != score;
  if (whitespace_before || whitespace_after || LooksLikeScore(score)) {
    if (error) {
      *error = "invalid hotword score; use 'word:3.5' without spaces: " + line;
    }
    return false;
  }

  entry->text = std::move(line);
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
  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    HotwordEntry entry;
    if (!ParseHotwordEntry(std::move(line), &entry, error)) {
      return false;
    }
    if (!entry.text.empty()) {
      entries->push_back(std::move(entry));
    }
  }
  if (error) {
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

  hotwords->clear();
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

  hotwords->clear();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i) {
      hotwords->push_back(',');
    }
    *hotwords += entries[i].text;
  }
  return true;
}

bool ValidateTransducerHotwordAssets(const ModelInfo& info, std::string* error) {
  const std::string modeling_unit = JsonString(info.model_config, "modeling_unit");
  if (modeling_unit.empty()) {
    if (error) {
      *error = "hotword-capable transducer is missing model.modeling_unit; "
               "reinstall or update the model";
    }
    return false;
  }
  if (modeling_unit != "cjkchar" && modeling_unit != "bpe" && modeling_unit != "bbpe" &&
      modeling_unit != "cjkchar+bpe") {
    if (error) {
      *error = "unsupported sherpa-onnx hotword modeling_unit '" + modeling_unit + "'";
    }
    return false;
  }

  if (modeling_unit == "bpe" || modeling_unit == "bbpe" || modeling_unit == "cjkchar+bpe") {
    const std::string bpe_vocab = info.File("bpe_vocab");
    if (bpe_vocab.empty() || !std::filesystem::is_regular_file(bpe_vocab)) {
      if (error) {
        *error = "modeling_unit '" + modeling_unit + "' requires model.bpe_vocab";
      }
      return false;
    }
    if (!ValidateBpeVocabulary(bpe_vocab, error)) {
      return false;
    }
  }

  if (error) {
    error->clear();
  }
  return true;
}

} // namespace vinput::daemon::asr
