#include "daemon/asr/hotword_utils.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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

  std::string text;
  std::string read_error;
  if (!vinput::file::ReadTextFile(file_path, &text, &read_error, kMaxHotwordFileBytes + 1)) {
    if (error) {
      *error = "failed to read hotwords file: " + path;
    }
    return false;
  }
  if (text.size() > kMaxHotwordFileBytes) {
    if (error) {
      *error = "hotwords file exceeds 1 MiB limit: " + path;
    }
    return false;
  }
  const auto utf8_res = vinput::str::ValidateUtf8Text(text, /*allow_control_chars=*/false,
                                                      /*allow_embedded_nul=*/false);
  if (!utf8_res.valid) {
    if (error != nullptr) {
      *error = "invalid hotwords file at byte " + std::to_string(utf8_res.error_offset) + ": " +
               utf8_res.error_reason;
    }
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
  return vinput::str::ParseFloatStrict(value);
}

bool LooksLikeScore(std::string_view value) {
  if (value.empty()) {
    return true;
  }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if ((first >= '0' && first <= '9') || first == '+' || first == '-' || first == '.') {
    return true;
  }

  return vinput::str::EqualsIgnoreCase(value, "nan") ||
         vinput::str::StartsWithIgnoreCase(value, "nan(") ||
         vinput::str::EqualsIgnoreCase(value, "inf") ||
         vinput::str::EqualsIgnoreCase(value, "infinity");
}

bool ParseHotwordEntry(std::string_view raw_line, HotwordEntry* entry, std::string* error) {
  const std::string_view line = vinput::str::TrimAsciiWhitespaceView(raw_line);
  if (line.empty()) {
    entry->text.clear();
    entry->score.clear();
    return true;
  }

  for (std::size_t i = 1; i < line.size(); ++i) {
    if (line[i] == ':' && vinput::str::IsAsciiWhitespace(line[i - 1])) {
      if (error) {
        *error = "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line);
      }
      return false;
    }
  }

  const auto pair = vinput::str::SplitPairRight(line, ':');
  if (!pair) {
    entry->text = std::string(line);
    entry->score.clear();
    return true;
  }

  const std::string_view raw_text = pair->first;
  const std::string_view raw_score = pair->second;
  const std::string_view score = vinput::str::TrimAsciiWhitespaceView(raw_score);
  const std::string_view text = vinput::str::TrimAsciiWhitespaceView(raw_text);
  if (IsValidScore(score)) {
    const bool whitespace_before =
        !raw_text.empty() && vinput::str::IsAsciiWhitespace(raw_text.back());
    const bool whitespace_after = raw_score != score;
    if (whitespace_before || whitespace_after) {
      if (error) {
        *error = "invalid hotword score; use 'word:3.5' without spaces: " + std::string(line);
      }
      return false;
    }
    if (text.empty()) {
      if (error) {
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
  const bool whitespace_before =
      !raw_text.empty() && vinput::str::IsAsciiWhitespace(raw_text.back());
  const bool whitespace_after = raw_score != score;
  if (whitespace_before || whitespace_after || LooksLikeScore(score)) {
    if (error) {
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
