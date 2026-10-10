#include "vad_trimmer.h"

#include <algorithm>
#include <cstdio>
#include <sherpa-onnx/c-api/c-api.h>

VadTrimmer::VadTrimmer() = default;

VadTrimmer::~VadTrimmer() {
  Shutdown();
}

bool VadTrimmer::Init(const std::string& model_path, int sample_rate, const std::string& provider,
                      const VadTrimParams& params, std::string* error) {
  if (vad_)
    return true;

  params_ = params;
  SherpaOnnxVadModelConfig config = {};
  config.silero_vad.model = model_path.c_str();
  config.silero_vad.threshold = params_.threshold;
  config.silero_vad.min_silence_duration = params_.min_silence_duration;
  config.silero_vad.min_speech_duration = params_.min_speech_duration;
  config.silero_vad.window_size = 512;
  config.silero_vad.max_speech_duration = 0.0F;
  config.sample_rate = sample_rate;
  config.num_threads = 1;
  config.provider = provider.c_str();
  config.debug = 0;

  vad_ = SherpaOnnxCreateVoiceActivityDetector(&config, 30.0F);
  if (!vad_) {
    if (error) {
      *error = "failed to create VAD from '" + model_path + "'";
    }
    return false;
  }

  sample_rate_ = sample_rate;
  fprintf(stderr,
          "vinput: VAD initialized from '%s' threshold=%.2f min_speech=%.2f "
          "min_silence=%.2f pad_ms=%d\n",
          model_path.c_str(), params_.threshold, params_.min_speech_duration,
          params_.min_silence_duration, params_.speech_pad_ms);
  return true;
}

std::vector<float> VadTrimmer::Trim(const std::vector<float>& samples, int /*sample_rate*/) {
  if (vad_ == nullptr || samples.empty()) {
    return samples;
  }

  const int n = static_cast<int>(samples.size());
  SherpaOnnxVoiceActivityDetectorReset(vad_);

  // Feed audio in window_size chunks
  const int window_size = 512;
  int offset = 0;
  for (; offset + window_size <= n; offset += window_size) {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, samples.data() + offset, window_size);
  }
  if (offset < n) {
    std::vector<float> padded_tail(window_size, 0.0F);
    const int remaining = n - offset;
    for (int i = 0; i < remaining; ++i) {
      padded_tail[i] = samples[offset + i];
    }
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, padded_tail.data(), window_size);
  }
  SherpaOnnxVoiceActivityDetectorFlush(vad_);

  // Determine global speech bounds across all detected segments.
  // We trim only leading and trailing silence outside the outer speech bounds;
  // internal pauses are fully preserved to retain natural acoustic context.
  int earliest_speech_start = -1;
  int latest_speech_end = -1;

  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
    const SherpaOnnxSpeechSegment* seg = SherpaOnnxVoiceActivityDetectorFront(vad_);
    if (seg != nullptr && seg->n > 0) {
      const int seg_start = static_cast<int>(seg->start);
      const int seg_end = seg_start + static_cast<int>(seg->n);
      if (earliest_speech_start < 0 || seg_start < earliest_speech_start) {
        earliest_speech_start = seg_start;
      }
      if (seg_end > latest_speech_end) {
        latest_speech_end = seg_end;
      }
    }
    if (seg != nullptr) {
      SherpaOnnxDestroySpeechSegment(seg);
    }
    SherpaOnnxVoiceActivityDetectorPop(vad_);
  }

  if (earliest_speech_start < 0 || latest_speech_end <= earliest_speech_start) {
    fprintf(stderr, "vinput: VAD found no speech, returning original audio\n");
    return samples;
  }

  const int padding_samples = std::max(
      0, static_cast<int>(static_cast<long long>(params_.speech_pad_ms) * sample_rate_ / 1000));
  const int cut_start = std::max(0, earliest_speech_start - padding_samples);
  const int cut_end = std::min(n, latest_speech_end + padding_samples);

  if (cut_start <= 0 && cut_end >= n) {
    return samples;
  }
  if (cut_end <= cut_start) {
    return samples;
  }

  std::vector<float> result(samples.begin() + cut_start, samples.begin() + cut_end);

  const int leading_removed = cut_start;
  const int trailing_removed = n - cut_end;
  fprintf(stderr,
          "vinput: VAD trimmed %d -> %zu samples leading_removed_ms=%d "
          "trailing_removed_ms=%d pad_ms=%d\n",
          n, result.size(), leading_removed * 1000 / sample_rate_,
          trailing_removed * 1000 / sample_rate_, params_.speech_pad_ms);
  return result;
}

bool VadTrimmer::Available() const {
  return vad_ != nullptr;
}

void VadTrimmer::Shutdown() {
  if (vad_) {
    SherpaOnnxDestroyVoiceActivityDetector(vad_);
    vad_ = nullptr;
  }
}
