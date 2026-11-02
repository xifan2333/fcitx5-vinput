#include "daemon/asr/vad_segmentation.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

std::vector<std::vector<float>>
CoalesceAndSliceSegments(std::vector<std::vector<float>> raw_segments, int sample_rate,
                         float max_segment_duration_sec, float min_merge_duration_sec) {
  if (sample_rate <= 0 || raw_segments.empty()) {
    return raw_segments;
  }

  const auto max_segment_samples =
      static_cast<std::size_t>(max_segment_duration_sec * static_cast<float>(sample_rate));
  const auto min_merge_samples =
      static_cast<std::size_t>(min_merge_duration_sec * static_cast<float>(sample_rate));

  if (max_segment_samples == 0) {
    return raw_segments;
  }

  std::vector<std::vector<float>> result;
  for (auto& seg : raw_segments) {
    if (seg.empty()) {
      continue;
    }

    if (seg.size() > max_segment_samples) {
      for (std::size_t offset = 0; offset < seg.size(); offset += max_segment_samples) {
        const std::size_t slice_len = std::min(max_segment_samples, seg.size() - offset);
        result.emplace_back(seg.begin() + static_cast<std::ptrdiff_t>(offset),
                            seg.begin() + static_cast<std::ptrdiff_t>(offset + slice_len));
      }
      continue;
    }

    if (!result.empty() && result.back().size() < min_merge_samples &&
        result.back().size() + seg.size() <= max_segment_samples) {
      result.back().insert(result.back().end(), seg.begin(), seg.end());
    } else {
      result.push_back(std::move(seg));
    }
  }

  return result;
}
