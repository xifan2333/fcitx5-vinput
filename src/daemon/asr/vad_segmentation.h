#pragma once

#include <vector>

/**
 * @brief Slices segments exceeding max_segment_duration_sec and coalesces short fragments.
 *
 * Slices any segment whose length exceeds max_segment_duration_sec into chunks of at most
 * max_segment_duration_sec. Adjacent short segments shorter than min_merge_duration_sec
 * are coalesced into a single segment as long as their combined duration does not exceed
 * max_segment_duration_sec. Empty segments are filtered out.
 *
 * @param raw_segments List of raw speech audio segments.
 * @param sample_rate Audio sample rate in Hz (e.g., 16000).
 * @param max_segment_duration_sec Maximum duration in seconds for any single segment
 * (default: 15.0s).
 * @param min_merge_duration_sec Minimum duration threshold in seconds below which short segments
 * coalesce (default: 0.5s).
 * @return Sliced and coalesced audio segments suitable for offline ASR inference.
 */
std::vector<std::vector<float>>
CoalesceAndSliceSegments(std::vector<std::vector<float>> raw_segments, int sample_rate,
                         float max_segment_duration_sec = 15.0F,
                         float min_merge_duration_sec = 0.50F);
