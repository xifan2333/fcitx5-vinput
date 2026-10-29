#include "vad_trimmer.h"

#include <algorithm>
#include <cstdint>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <string>
#include <utility>
#include <vector>

#include "common/utils/debug_log.h"

VadTrimmer::VadTrimmer() = default;
VadTrimmer::~VadTrimmer() = default;
VadTrimmer::VadTrimmer(VadTrimmer&&) noexcept = default;
VadTrimmer& VadTrimmer::operator=(VadTrimmer&&) noexcept = default;

bool VadTrimmer::Init(const std::string& model_path, int sample_rate, const std::string& provider,
                      const VadTrimParams& params, std::string* error) {
  if (Available()) {
    return true;
  }

  params_ = params;
  sherpa_onnx::cxx::VadModelConfig config;
  config.silero_vad.model = model_path;
  config.silero_vad.threshold = params_.threshold;
  config.silero_vad.min_silence_duration = params_.min_silence_duration;
  config.silero_vad.min_speech_duration = params_.min_speech_duration;
  config.silero_vad.window_size = 512;
  config.silero_vad.max_speech_duration = 0.0F;
  config.sample_rate = sample_rate;
  config.num_threads = 1;
  config.provider = provider;
  config.debug = false;

  auto vad = sherpa_onnx::cxx::VoiceActivityDetector::Create(config, 30.0F);
  if (vad.Get() == nullptr) {
    if (error != nullptr) {
      *error = "failed to create VAD from '" + model_path + "'";
    }
    return false;
  }

  vad_ = std::move(vad);
  sample_rate_ = sample_rate;
  VLOG_INFO("VAD initialized from '{}' threshold={:.2f} min_speech={:.2f} "
            "min_silence={:.2f} pad_ms={}",
            model_path, params_.threshold, params_.min_speech_duration,
            params_.min_silence_duration, params_.speech_pad_ms);
  return true;
}

std::vector<float> VadTrimmer::Trim(const std::vector<float>& samples, int /*sample_rate*/) {
  if (!vad_.has_value() || vad_->Get() == nullptr || samples.empty()) {
    return samples;
  }

  auto& vad = *vad_;
  const int n = static_cast<int>(samples.size());
  vad.Reset();

  // Feed audio in window_size chunks
  constexpr int kWindowSize = 512;
  int offset = 0;
  for (; offset + kWindowSize <= n; offset += kWindowSize) {
    vad.AcceptWaveform(samples.data() + offset, kWindowSize);
  }
  if (offset < n) {
    std::vector<float> padded_tail(kWindowSize, 0.0F);
    const int remaining = n - offset;
    for (int i = 0; i < remaining; ++i) {
      padded_tail[i] = samples[offset + i];
    }
    vad.AcceptWaveform(padded_tail.data(), kWindowSize);
  }
  vad.Flush();

  // Determine global speech bounds across all detected segments.
  // We trim only leading and trailing silence outside the outer speech bounds;
  // internal pauses are fully preserved to retain natural acoustic context.
  int64_t earliest_speech_start = -1;
  int64_t latest_speech_end = -1;

  while (!vad.IsEmpty()) {
    auto seg = vad.Front();
    if (!seg.samples.empty()) {
      const int64_t seg_start = seg.start;
      const int64_t seg_end = seg_start + static_cast<int64_t>(seg.samples.size());
      if (earliest_speech_start < 0 || seg_start < earliest_speech_start) {
        earliest_speech_start = seg_start;
      }
      latest_speech_end = std::max(latest_speech_end, seg_end);
    }
    vad.Pop();
  }

  if (earliest_speech_start < 0) {
    VLOG_DEBUG("VAD found no speech, returning empty audio");
    return {};
  }
  if (latest_speech_end <= earliest_speech_start) {
    VLOG_WARN("VAD produced invalid speech bounds, returning original audio");
    return samples;
  }

  const int64_t padding_samples =
      std::max(int64_t{0}, static_cast<int64_t>(params_.speech_pad_ms) * sample_rate_ / 1000);
  const auto total_n = static_cast<int64_t>(n);
  const int64_t cut_start = std::max(int64_t{0}, earliest_speech_start - padding_samples);
  const int64_t cut_end = std::min(total_n, latest_speech_end + padding_samples);

  if (cut_start <= 0 && cut_end >= total_n) {
    return samples;
  }
  if (cut_end <= cut_start) {
    return samples;
  }

  std::vector<float> result(samples.begin() + cut_start, samples.begin() + cut_end);

  const int64_t leading_removed = cut_start;
  const int64_t trailing_removed = total_n - cut_end;
  VLOG_DEBUG("VAD trimmed {} -> {} samples leading_removed_ms={} "
             "trailing_removed_ms={} pad_ms={}",
             n, result.size(), leading_removed * 1000 / sample_rate_,
             trailing_removed * 1000 / sample_rate_, params_.speech_pad_ms);
  return result;
}

bool VadTrimmer::Available() const {
  return vad_.has_value() && vad_->Get() != nullptr;
}

void VadTrimmer::Shutdown() {
  vad_.reset();
}
