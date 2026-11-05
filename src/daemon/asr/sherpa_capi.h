#pragma once

// RAII wrappers over the sherpa-onnx C API.
//
// The official sherpa-onnx prebuilt `libsherpa-onnx-cxx-api.so` is compiled
// with the old C++ string ABI (`_GLIBCXX_USE_CXX11_ABI=0`), so its C++ config
// structs (which contain std::string) are ABI-incompatible with a downstream
// built with the default ABI. The C API, by contrast, exposes only `const char*`
// and plain structs, so it has no std::string ABI surface.
//
// These wrappers give the calling code the same ergonomics as the C++ API
// (RAII ownership, small methods, nullopt on failure) while talking to the C
// API underneath. They deliberately cover only the subset vinput uses, and
// hide every raw handle's create/destroy lifecycle behind a movable,
// non-copyable object. The bodies are thin one-line forwards, so this is
// header-only.

#include <cstdint>
#include <optional>
#include <sherpa-onnx/c-api/c-api.h>
#include <string>
#include <vector>

namespace vinput::daemon::asr::sherpa {

struct SpeechSegment {
  int64_t start = 0;
  std::vector<float> samples;
};

// ---------------------------------------------------------------------------
// Offline (non-streaming) ASR
// ---------------------------------------------------------------------------

class OfflineStream {
  friend class OfflineRecognizer;

public:
  OfflineStream() = default;
  ~OfflineStream() {
    if (handle_ != nullptr) {
      SherpaOnnxDestroyOfflineStream(handle_);
    }
  }
  OfflineStream(OfflineStream&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  OfflineStream& operator=(OfflineStream&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        SherpaOnnxDestroyOfflineStream(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  OfflineStream(const OfflineStream&) = delete;
  OfflineStream& operator=(const OfflineStream&) = delete;

  [[nodiscard]] bool Valid() const { return handle_ != nullptr; }
  void AcceptWaveform(int32_t sample_rate, const float* samples, int32_t n) const {
    SherpaOnnxAcceptWaveformOffline(handle_, sample_rate, samples, n);
  }

private:
  explicit OfflineStream(const SherpaOnnxOfflineStream* handle) : handle_(handle) {}
  const SherpaOnnxOfflineStream* handle_ = nullptr;
};

class OfflineRecognizer {
public:
  static std::optional<OfflineRecognizer> Create(
      const SherpaOnnxOfflineRecognizerConfig& config) {
    const auto* handle = SherpaOnnxCreateOfflineRecognizer(&config);
    if (handle == nullptr) {
      return std::nullopt;
    }
    return OfflineRecognizer(handle);
  }

  OfflineRecognizer() = default;
  ~OfflineRecognizer() {
    if (handle_ != nullptr) {
      SherpaOnnxDestroyOfflineRecognizer(handle_);
    }
  }
  OfflineRecognizer(OfflineRecognizer&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  OfflineRecognizer& operator=(OfflineRecognizer&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        SherpaOnnxDestroyOfflineRecognizer(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  OfflineRecognizer(const OfflineRecognizer&) = delete;
  OfflineRecognizer& operator=(const OfflineRecognizer&) = delete;

  [[nodiscard]] bool Valid() const { return handle_ != nullptr; }

  OfflineStream CreateStream() const {
    return OfflineStream(SherpaOnnxCreateOfflineStream(handle_));
  }
  void Decode(const OfflineStream& stream) const {
    SherpaOnnxDecodeOfflineStream(handle_, stream.handle_);
  }
  [[nodiscard]] std::string GetText(const OfflineStream& stream) const {
    const auto* result = SherpaOnnxGetOfflineStreamResult(stream.handle_);
    std::string text;
    if (result != nullptr && result->text != nullptr) {
      text = result->text;
    }
    if (result != nullptr) {
      SherpaOnnxDestroyOfflineRecognizerResult(result);
    }
    return text;
  }

private:
  explicit OfflineRecognizer(const SherpaOnnxOfflineRecognizer* handle) : handle_(handle) {}
  const SherpaOnnxOfflineRecognizer* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// Streaming (online) ASR
// ---------------------------------------------------------------------------

class OnlineStream {
  friend class OnlineRecognizer;

public:
  OnlineStream() = default;
  ~OnlineStream() {
    if (handle_ != nullptr) {
      SherpaOnnxDestroyOnlineStream(handle_);
    }
  }
  OnlineStream(OnlineStream&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  OnlineStream& operator=(OnlineStream&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        SherpaOnnxDestroyOnlineStream(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  OnlineStream(const OnlineStream&) = delete;
  OnlineStream& operator=(const OnlineStream&) = delete;

  [[nodiscard]] bool Valid() const { return handle_ != nullptr; }
  void AcceptWaveform(int32_t sample_rate, const float* samples, int32_t n) const {
    SherpaOnnxOnlineStreamAcceptWaveform(handle_, sample_rate, samples, n);
  }
  void InputFinished() const { SherpaOnnxOnlineStreamInputFinished(handle_); }

private:
  explicit OnlineStream(const SherpaOnnxOnlineStream* handle) : handle_(handle) {}
  const SherpaOnnxOnlineStream* handle_ = nullptr;
};

class OnlineRecognizer {
public:
  static std::optional<OnlineRecognizer> Create(const SherpaOnnxOnlineRecognizerConfig& config) {
    const auto* handle = SherpaOnnxCreateOnlineRecognizer(&config);
    if (handle == nullptr) {
      return std::nullopt;
    }
    return OnlineRecognizer(handle);
  }

  OnlineRecognizer() = default;
  ~OnlineRecognizer() {
    if (handle_ != nullptr) {
      SherpaOnnxDestroyOnlineRecognizer(handle_);
    }
  }
  OnlineRecognizer(OnlineRecognizer&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  OnlineRecognizer& operator=(OnlineRecognizer&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        SherpaOnnxDestroyOnlineRecognizer(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  OnlineRecognizer(const OnlineRecognizer&) = delete;
  OnlineRecognizer& operator=(const OnlineRecognizer&) = delete;

  [[nodiscard]] bool Valid() const { return handle_ != nullptr; }

  OnlineStream CreateStream() const {
    return OnlineStream(SherpaOnnxCreateOnlineStream(handle_));
  }
  bool IsReady(const OnlineStream& stream) const {
    return SherpaOnnxIsOnlineStreamReady(handle_, stream.handle_) != 0;
  }
  void Decode(const OnlineStream& stream) const {
    SherpaOnnxDecodeOnlineStream(handle_, stream.handle_);
  }
  [[nodiscard]] std::string GetText(const OnlineStream& stream) const {
    const auto* result = SherpaOnnxGetOnlineStreamResult(handle_, stream.handle_);
    std::string text;
    if (result != nullptr && result->text != nullptr) {
      text = result->text;
    }
    if (result != nullptr) {
      SherpaOnnxDestroyOnlineRecognizerResult(result);
    }
    return text;
  }

private:
  explicit OnlineRecognizer(const SherpaOnnxOnlineRecognizer* handle) : handle_(handle) {}
  const SherpaOnnxOnlineRecognizer* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// Voice activity detection
// ---------------------------------------------------------------------------

class VoiceActivityDetector {
public:
  static std::optional<VoiceActivityDetector> Create(const SherpaOnnxVadModelConfig& config,
                                                     float buffer_size_in_seconds) {
    const auto* handle = SherpaOnnxCreateVoiceActivityDetector(&config, buffer_size_in_seconds);
    if (handle == nullptr) {
      return std::nullopt;
    }
    return VoiceActivityDetector(handle);
  }

  VoiceActivityDetector() = default;
  ~VoiceActivityDetector() {
    if (handle_ != nullptr) {
      SherpaOnnxDestroyVoiceActivityDetector(handle_);
    }
  }
  VoiceActivityDetector(VoiceActivityDetector&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  VoiceActivityDetector& operator=(VoiceActivityDetector&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        SherpaOnnxDestroyVoiceActivityDetector(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  VoiceActivityDetector(const VoiceActivityDetector&) = delete;
  VoiceActivityDetector& operator=(const VoiceActivityDetector&) = delete;

  [[nodiscard]] bool Valid() const { return handle_ != nullptr; }

  void AcceptWaveform(const float* samples, int32_t n) const {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(handle_, samples, n);
  }
  void Reset() const { SherpaOnnxVoiceActivityDetectorReset(handle_); }
  void Flush() const { SherpaOnnxVoiceActivityDetectorFlush(handle_); }
  [[nodiscard]] bool Empty() const { return SherpaOnnxVoiceActivityDetectorEmpty(handle_) != 0; }
  [[nodiscard]] SpeechSegment Front() const {
    const auto* segment = SherpaOnnxVoiceActivityDetectorFront(handle_);
    SpeechSegment result;
    if (segment == nullptr) {
      return result;
    }
    result.start = segment->start;
    result.samples.assign(segment->samples, segment->samples + segment->n);
    SherpaOnnxDestroySpeechSegment(segment);
    return result;
  }
  void Pop() const { SherpaOnnxVoiceActivityDetectorPop(handle_); }

private:
  explicit VoiceActivityDetector(const SherpaOnnxVoiceActivityDetector* handle) : handle_(handle) {}
  const SherpaOnnxVoiceActivityDetector* handle_ = nullptr;
};

} // namespace vinput::daemon::asr::sherpa
