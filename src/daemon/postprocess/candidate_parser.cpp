#include "candidate_parser.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/json/json_access.h"
#include "common/utils/string_utils.h"

namespace vinput::postprocess {

namespace {

std::string_view StripMarkdownFence(std::string_view text) {
  text = vinput::str::Trim(text);
  if (text.starts_with("```")) {
    const auto newline_pos = text.find('\n');
    if (newline_pos != std::string_view::npos) {
      const auto body = text.substr(newline_pos + 1);
      const auto trimmed_body = vinput::str::Trim(body);
      if (trimmed_body.ends_with("```")) {
        return vinput::str::Trim(trimmed_body.substr(0, trimmed_body.size() - 3));
      }
    }
  }
  return text;
}

std::string_view StripBulletOrNumber(std::string_view line) {
  if (line.starts_with("- ") || line.starts_with("* ")) {
    return vinput::str::Trim(line.substr(2));
  }
  if (line.size() >= 3 && std::isdigit(static_cast<unsigned char>(line[0])) != 0 &&
      line[1] == '.' && line[2] == ' ') {
    return vinput::str::Trim(line.substr(3));
  }
  if (line.size() >= 4 && std::isdigit(static_cast<unsigned char>(line[0])) != 0 &&
      std::isdigit(static_cast<unsigned char>(line[1])) != 0 && line[2] == '.' && line[3] == ' ') {
    return vinput::str::Trim(line.substr(4));
  }
  return line;
}

std::optional<std::vector<std::string>> TryExtractJsonCandidates(std::string_view content,
                                                                 int max_candidates) {
  glz::json_t json_val;
  if (bool(glz::read_json(json_val, content)) || !json_val.is_object()) {
    return std::nullopt;
  }

  const auto* candidate_array = vinput::json::Find(json_val, "candidates");
  if (candidate_array == nullptr || !candidate_array->is_array()) {
    return std::nullopt;
  }

  std::vector<std::string> candidates;
  for (const auto& item : candidate_array->get_array()) {
    if (!item.is_string()) {
      continue;
    }
    const auto trimmed = vinput::str::Trim(item.get_string());
    if (trimmed.empty()) {
      continue;
    }
    candidates.emplace_back(trimmed);
    if (std::cmp_greater_equal(candidates.size(), max_candidates)) {
      break;
    }
  }
  return candidates;
}

} // namespace

std::vector<std::string> ExtractCandidatesAuto(std::string_view raw, int max_candidates) {
  if (max_candidates <= 0) {
    return {};
  }

  const auto content = StripMarkdownFence(raw);
  if (content.empty()) {
    return {};
  }

  // Format 1: JSON object with {"candidates": ["..."]}
  if (auto json_candidates = TryExtractJsonCandidates(content, max_candidates)) {
    return std::move(*json_candidates);
  }

  // Format 2: Single candidate direct text when max_candidates == 1
  if (max_candidates == 1) {
    const auto trimmed = vinput::str::Trim(content);
    if (!trimmed.empty()) {
      return {std::string(trimmed)};
    }
    return {};
  }

  // Format 3: Line stream (text1\ntext2\n...)
  const auto lines = vinput::str::Split(content, '\n');
  std::vector<std::string> candidates;
  candidates.reserve(std::min<std::size_t>(lines.size(), static_cast<std::size_t>(max_candidates)));
  for (const auto& line : lines) {
    auto trimmed = vinput::str::Trim(line);
    trimmed = StripBulletOrNumber(trimmed);
    if (!trimmed.empty()) {
      candidates.emplace_back(trimmed);
      if (std::cmp_greater_equal(candidates.size(), max_candidates)) {
        break;
      }
    }
  }

  return candidates;
}

} // namespace vinput::postprocess
