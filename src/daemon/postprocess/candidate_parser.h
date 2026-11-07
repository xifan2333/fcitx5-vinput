#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vinput::postprocess {

/// Extracts candidates from raw model or filter output text.
///
/// Supported formats:
/// 1. JSON object: `{"candidates": ["cand1", "cand2"]}` (optionally wrapped in markdown code
/// fences)
/// 2. Line stream (`text1\ntext2\n`): each non-empty trimmed line is a candidate
/// 3. Single text (when max_candidates <= 1): entire stripped text is the candidate
///
/// Returns at most `max_candidates` items.
std::vector<std::string> ExtractCandidatesAuto(std::string_view raw, int max_candidates);

} // namespace vinput::postprocess
