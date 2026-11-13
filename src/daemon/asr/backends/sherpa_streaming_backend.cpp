#include "daemon/asr/backends/sherpa_streaming_backend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glaze/json/json_t.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <sherpa-onnx/c-api/c-api.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/utils/debug_log.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/asr_config.h"
#include "daemon/asr/hotword_utils.h"
#include "daemon/asr/local_asr_model_loader.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/sherpa_capi.h"
#include "daemon/asr/sherpa_json_helpers.h"

namespace vinput::daemon::asr {

namespace {

constexpr int32_t kStreamingSampleRate = 16000;
// Streaming encoders (Zipformer / Paraformer / CTC) require ~0.66s of trailing
// silence before InputFinished() to flush final tokens out of their right-context
// lookahead window (see upstream sherpa-onnx online-decode-files examples).
constexpr std::size_t kTailPaddingSamples = (std::size_t{16000} * 66) / 100;

int ChooseNumThreads(const glz::json_t& model_cfg, int runtime_default) {
  if (runtime_default > 0) {
    return runtime_default;
  }
  return JsonInt(model_cfg, "num_threads", 1);
}

class SherpaStreamingSession : public RecognitionSession {
public:
  SherpaStreamingSession(const sherpa::OnlineRecognizer* recognizer, sherpa::OnlineStream stream)
      : recognizer_(recognizer), stream_(std::move(stream)) {}

  SherpaStreamingSession(const SherpaStreamingSession&) = delete;
  SherpaStreamingSession& operator=(const SherpaStreamingSession&) = delete;
  SherpaStreamingSession(SherpaStreamingSession&&) = delete;
  SherpaStreamingSession& operator=(SherpaStreamingSession&&) = delete;

  ~SherpaStreamingSession() override { Reset(); }

  using RecognitionSession::PushAudio;

  bool PushAudio(std::span<const float> samples, std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        *error = "Recognition session already finished.";
      }
      return false;
    }

    if (!stream_.Valid() || recognizer_ == nullptr || !recognizer_->Valid()) {
      if (error != nullptr) {
        *error = "Streaming recognizer is not initialized.";
      }
      return false;
    }

    if (samples.empty()) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    UpdateAudioStats(samples);

    stream_.AcceptWaveform(16000, samples.data(), static_cast<int32_t>(samples.size()));
    DecodeAvailable();

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    std::vector<float> samples(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      samples[i] = static_cast<float>(pcm[i]) / 32768.0F;
    }
    return PushAudio(std::span<const float>(samples), error);
  }

  bool Finish(std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    if (!stream_.Valid() || recognizer_ == nullptr || !recognizer_->Valid()) {
      if (error != nullptr) {
        *error = "Streaming recognizer is not initialized.";
      }
      return false;
    }

    finished_ = true;
    if (total_samples_ > 0) {
      const std::vector<float> tail_padding(kTailPaddingSamples, 0.0F);
      stream_.AcceptWaveform(kStreamingSampleRate, tail_padding.data(),
                             static_cast<int32_t>(tail_padding.size()));
    }
    stream_.InputFinished();
    DecodeAvailable();

    std::string text = GetCurrentText();
    VLOG_DEBUG("streaming finish current result bytes={}", text.size());
    if (!text.empty()) {
      if (text != last_partial_text_) {
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
        last_partial_text_ = text;
      }
      events_.push_back({RecognitionEventKind::FinalText, std::move(text), {}});
      VLOG_DEBUG("streaming queued final result");
    } else {
      const double rms =
          total_samples_ > 0 ? std::sqrt(sum_squares_ / static_cast<double>(total_samples_)) : 0.0;
      VLOG_DEBUG("streaming finish produced empty text samples={} peak={:.4f} rms={:.4f}",
                 total_samples_, peak_abs_, rms);
    }
    events_.push_back({RecognitionEventKind::Completed, {}, {}});

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void Cancel() override {
    finished_ = true;
    events_.clear();
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    Reset();
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  void UpdateAudioStats(std::span<const float> samples) {
    total_samples_ += samples.size();
    for (const float sample : samples) {
      const double amplitude = std::fabs(static_cast<double>(sample));
      peak_abs_ = std::max(peak_abs_, amplitude);
      sum_squares_ += amplitude * amplitude;
    }
  }

  void DecodeAvailable() {
    if (!stream_.Valid() || recognizer_ == nullptr || !recognizer_->Valid()) {
      return;
    }

    int decode_iterations = 0;
    while (recognizer_->IsReady(stream_)) {
      ++decode_iterations;
      recognizer_->Decode(stream_);
      const std::string text = GetCurrentText();
      if (!text.empty() && text != last_partial_text_) {
        last_partial_text_ = text;
        VLOG_DEBUG("streaming partial result bytes={} decode_iterations={}", text.size(),
                   decode_iterations);
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
      }
    }
    if (decode_iterations > 0) {
      VLOG_DEBUG("streaming decode loop completed iterations={}", decode_iterations);
    }
  }

  [[nodiscard]] std::string GetCurrentText() const {
    if (!stream_.Valid() || recognizer_ == nullptr || !recognizer_->Valid()) {
      return {};
    }

    std::string text = std::string(vinput::str::Trim(recognizer_->GetText(stream_)));
    VLOG_DEBUG("streaming current result bytes={}", text.size());
    return text;
  }

  void Reset() {
    stream_ = sherpa::OnlineStream{};
    recognizer_ = nullptr;
  }

  const sherpa::OnlineRecognizer* recognizer_ = nullptr;
  sherpa::OnlineStream stream_;
  bool finished_ = false;
  std::size_t total_samples_ = 0;
  double peak_abs_ = 0.0;
  double sum_squares_ = 0.0;
  std::string last_partial_text_;
  std::vector<RecognitionEvent> events_;
};

struct StreamingRecognizerConfigContext {
  std::string tokens_path;
  std::string f_model;
  std::string f_encoder;
  std::string f_decoder;
  std::string f_joiner;
  std::string f_bpe_vocab;
  std::string f_rule_fsts;
  std::string f_rule_fars;
  std::string f_hotwords_file;
  std::string f_graph;
  std::string f_hr_lexicon;
  std::string f_hr_rule_fsts;
  std::string explicit_model_type;
  std::string modeling_unit;
  std::string tokens_buf;
  std::string hotwords_buf;
  std::string selected_hotwords_file;
  std::string normalized_transducer_hotwords;
  std::string p_provider;
  std::string p_decoding_method;
  bool use_hotwords_buf = false;
};

void ResolveAssetPaths(const ModelInfo& info, StreamingRecognizerConfigContext& ctx) {
  const auto& recognizer_cfg = info.recognizer_config;
  const auto& model_cfg = info.model_config;

  ctx.tokens_path = info.File("tokens");
  ctx.f_model = info.File("model");
  ctx.f_encoder = info.File("encoder");
  ctx.f_decoder = info.File("decoder");
  ctx.f_joiner = info.File("joiner");
  ctx.f_bpe_vocab = info.File("bpe_vocab");
  ctx.f_rule_fsts = info.File("rule_fsts");
  ctx.f_rule_fars = info.File("rule_fars");
  ctx.f_hotwords_file = info.File("hotwords_file");
  ctx.f_graph = info.File("graph");
  ctx.f_hr_lexicon = info.File("hr_lexicon");
  ctx.f_hr_rule_fsts = info.File("hr_rule_fsts");
  ctx.explicit_model_type = JsonString(model_cfg, "model_type");
  ctx.modeling_unit = JsonString(model_cfg, "modeling_unit");
  ctx.tokens_buf = JsonString(model_cfg, "tokens_buf");
  ctx.hotwords_buf = JsonString(recognizer_cfg, "hotwords_buf");
}

void ConfigureCommonSettings(const ModelInfo& info, const AsrConfig& asr_config,
                             StreamingRecognizerConfigContext& ctx,
                             SherpaOnnxOnlineRecognizerConfig& config) {
  const auto& recognizer_cfg = info.recognizer_config;
  const auto& model_cfg = info.model_config;
  const auto& feat_cfg = vinput::json::Object(recognizer_cfg, "feat_config");

  config.feat_config.sample_rate = JsonInt(feat_cfg, "sample_rate", 16000);
  config.feat_config.feature_dim = JsonInt(feat_cfg, "feature_dim", 80);

  config.model_config.tokens = ctx.tokens_path.c_str();
  config.model_config.num_threads = ChooseNumThreads(model_cfg, asr_config.thread_num);
  ctx.p_provider = JsonString(model_cfg, "provider", "cpu");
  config.model_config.provider = ctx.p_provider.c_str();
  ctx.p_decoding_method = JsonString(recognizer_cfg, "decoding_method", "greedy_search");
  config.decoding_method = ctx.p_decoding_method.c_str();
  config.max_active_paths = JsonInt(recognizer_cfg, "max_active_paths", 4);
  config.blank_penalty = JsonFloat(recognizer_cfg, "blank_penalty", 0.0F);
  config.enable_endpoint = JsonBool(recognizer_cfg, "enable_endpoint", true) ? 1 : 0;
  config.rule1_min_trailing_silence = JsonFloat(recognizer_cfg, "rule1_min_trailing_silence", 2.4F);
  config.rule2_min_trailing_silence = JsonFloat(recognizer_cfg, "rule2_min_trailing_silence", 1.2F);
  config.rule3_min_utterance_length =
      JsonFloat(recognizer_cfg, "rule3_min_utterance_length", 20.0F);
  config.hotwords_score = JsonFloat(recognizer_cfg, "hotwords_score", 1.5F);

  if (!ctx.explicit_model_type.empty()) {
    config.model_config.model_type = ctx.explicit_model_type.c_str();
  }
  if (!ctx.modeling_unit.empty()) {
    config.model_config.modeling_unit = ctx.modeling_unit.c_str();
  }
  if (!ctx.f_bpe_vocab.empty()) {
    config.model_config.bpe_vocab = ctx.f_bpe_vocab.c_str();
  }
  if (!ctx.tokens_buf.empty()) {
    config.model_config.tokens_buf = ctx.tokens_buf.c_str();
    config.model_config.tokens_buf_size = static_cast<int32_t>(ctx.tokens_buf.size());
  }
  if (!ctx.f_rule_fsts.empty()) {
    config.rule_fsts = ctx.f_rule_fsts.c_str();
  }
  if (!ctx.f_rule_fars.empty()) {
    config.rule_fars = ctx.f_rule_fars.c_str();
  }
  if (!ctx.f_graph.empty()) {
    config.ctc_fst_decoder_config.graph = ctx.f_graph.c_str();
    config.ctc_fst_decoder_config.max_active =
        JsonInt(vinput::json::Object(recognizer_cfg, "ctc_fst_decoder_config"), "max_active", 3000);
  }
  if (!ctx.f_hr_lexicon.empty()) {
    config.hr.lexicon = ctx.f_hr_lexicon.c_str();
  }
  if (!ctx.f_hr_rule_fsts.empty()) {
    config.hr.rule_fsts = ctx.f_hr_rule_fsts.c_str();
  }
}

bool ResolveHotwordsBuffer(const ModelInfo& info, const AsrConfig& asr_config,
                           StreamingRecognizerConfigContext& ctx, std::string* error) {
  const bool provider_hotwords_configured =
      info.supports_hotwords && !asr_config.hotwords_file.empty();
  if (provider_hotwords_configured && !IsTransducerHotwordFamily(info.family)) {
    if (error != nullptr) {
      *error = "model family '" + info.family +
               "' does not expose streaming ASR hotwords in sherpa-onnx";
    }
    return false;
  }

  if (!info.supports_hotwords || !IsTransducerHotwordFamily(info.family)) {
    return true;
  }

  ctx.selected_hotwords_file =
      !asr_config.hotwords_file.empty() ? asr_config.hotwords_file : ctx.f_hotwords_file;
  bool hotwords_active = false;
  if (!ctx.selected_hotwords_file.empty()) {
    if (!LoadTransducerHotwords(ctx.selected_hotwords_file, &ctx.normalized_transducer_hotwords,
                                error)) {
      return false;
    }
    hotwords_active = !ctx.normalized_transducer_hotwords.empty();
  } else if (!ctx.hotwords_buf.empty()) {
    ctx.use_hotwords_buf = true;
    hotwords_active = true;
  }

  if (hotwords_active && !ValidateTransducerHotwordAssets(info, error)) {
    return false;
  }
  return true;
}

void ApplyHotwordsConfig(const StreamingRecognizerConfigContext& ctx,
                         SherpaOnnxOnlineRecognizerConfig& config) {
  if (!ctx.normalized_transducer_hotwords.empty()) {
    config.hotwords_buf = ctx.normalized_transducer_hotwords.c_str();
    config.hotwords_buf_size = static_cast<int32_t>(ctx.normalized_transducer_hotwords.size());
    config.decoding_method = "modified_beam_search";
    return;
  }
  if (ctx.use_hotwords_buf) {
    config.hotwords_buf = ctx.hotwords_buf.c_str();
    config.hotwords_buf_size = static_cast<int32_t>(ctx.hotwords_buf.size());
    config.decoding_method = "modified_beam_search";
  }
}

bool ConfigureHotwords(const ModelInfo& info, const AsrConfig& asr_config,
                       StreamingRecognizerConfigContext& ctx,
                       SherpaOnnxOnlineRecognizerConfig& config, std::string* error) {
  if (!ResolveHotwordsBuffer(info, asr_config, ctx, error)) {
    return false;
  }
  ApplyHotwordsConfig(ctx, config);
  return true;
}

bool ConfigureModelFamily(const ModelInfo& info, const StreamingRecognizerConfigContext& ctx,
                          SherpaOnnxOnlineRecognizerConfig& config, std::string* error) {
  if (info.family == "transducer") {
    config.model_config.transducer.encoder = ctx.f_encoder.c_str();
    config.model_config.transducer.decoder = ctx.f_decoder.c_str();
    config.model_config.transducer.joiner = ctx.f_joiner.c_str();
    return true;
  }
  if (info.family == "zipformer2_ctc") {
    config.model_config.zipformer2_ctc.model = ctx.f_model.c_str();
    return true;
  }
  if (info.family == "paraformer") {
    config.model_config.paraformer.encoder = ctx.f_encoder.c_str();
    config.model_config.paraformer.decoder = ctx.f_decoder.c_str();
    return true;
  }
  if (info.family == "nemo_ctc") {
    config.model_config.nemo_ctc.model = ctx.f_model.c_str();
    return true;
  }
  if (info.family == "t_one_ctc") {
    config.model_config.t_one_ctc.model = ctx.f_model.c_str();
    return true;
  }
  if (error != nullptr) {
    *error = "Unsupported sherpa-streaming model family '" + info.family + "'";
  }
  return false;
}

class SherpaStreamingBackend : public AsrBackend {
public:
  SherpaStreamingBackend(ModelInfo model_info, AsrConfig asr_config, std::string provider_id)
      : model_info_(std::move(model_info)), asr_config_(std::move(asr_config)),
        provider_id_(std::move(provider_id)) {}

  SherpaStreamingBackend(const SherpaStreamingBackend&) = delete;
  SherpaStreamingBackend& operator=(const SherpaStreamingBackend&) = delete;
  SherpaStreamingBackend(SherpaStreamingBackend&&) = delete;
  SherpaStreamingBackend& operator=(SherpaStreamingBackend&&) = delete;

  ~SherpaStreamingBackend() override = default;

  [[nodiscard]] BackendDescriptor Describe() const override {
    BackendDescriptor descriptor;
    descriptor.provider_id = provider_id_;
    descriptor.provider_type = vinput::asr::kLocalProviderType;
    descriptor.backend_id = "sherpa-streaming";
    descriptor.capabilities.audio_delivery_mode = AudioDeliveryMode::Chunked;
    descriptor.capabilities.supports_streaming = true;
    descriptor.capabilities.supports_partial_results = true;
    descriptor.capabilities.supports_hotwords = model_info_.supports_hotwords;
    return descriptor;
  }

  std::unique_ptr<RecognitionSession> CreateSession(std::string* error) override {
    const std::scoped_lock lock(recognizer_mutex_);
    if (!InitializeRecognizerLocked(error) || !recognizer_.has_value() || !recognizer_->Valid()) {
      return nullptr;
    }

    auto stream = recognizer_->CreateStream();
    if (!stream.Valid()) {
      if (error != nullptr) {
        *error = "failed to create sherpa-streaming stream";
      }
      return nullptr;
    }

    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<SherpaStreamingSession>(&*recognizer_, std::move(stream));
  }

private:
  bool InitializeRecognizerLocked(std::string* error) {
    if (recognizer_.has_value() && recognizer_->Valid()) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    StreamingRecognizerConfigContext ctx;
    ResolveAssetPaths(model_info_, ctx);

    SherpaOnnxOnlineRecognizerConfig config = {};
    ConfigureCommonSettings(model_info_, asr_config_, ctx, config);

    if (!ConfigureHotwords(model_info_, asr_config_, ctx, config, error)) {
      return false;
    }

    if (!ConfigureModelFamily(model_info_, ctx, config, error)) {
      return false;
    }

    auto recognizer = sherpa::OnlineRecognizer::Create(config);
    if (!recognizer.has_value()) {
      if (error != nullptr) {
        *error =
            "failed to create sherpa-streaming recognizer for family '" + model_info_.family + "'";
      }
      return false;
    }

    recognizer_ = std::move(recognizer);

    VLOG_INFO(
        "sherpa-onnx streaming ASR initialized successfully (family: {}, lang: {}, threads: {})",
        model_info_.family, asr_config_.language, config.model_config.num_threads);

    WarmupRecognizerLocked();

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void WarmupRecognizerLocked() {
    if (!recognizer_.has_value() || !recognizer_->Valid()) {
      return;
    }

    auto stream = recognizer_->CreateStream();
    if (!stream.Valid()) {
      VLOG_WARN("streaming warmup skipped (stream create failed)");
      return;
    }

    std::vector<float> silence(16000 / 5, 0.0F);
    stream.AcceptWaveform(16000, silence.data(), static_cast<int32_t>(silence.size()));
    while (recognizer_->IsReady(stream)) {
      recognizer_->Decode(stream);
    }
    stream.InputFinished();
    while (recognizer_->IsReady(stream)) {
      recognizer_->Decode(stream);
    }

    VLOG_DEBUG("streaming recognizer warmup completed");
  }

  ModelInfo model_info_;
  AsrConfig asr_config_;
  std::string provider_id_;
  std::mutex recognizer_mutex_;
  std::optional<sherpa::OnlineRecognizer> recognizer_;
};

} // namespace

std::unique_ptr<AsrBackend> CreateSherpaStreamingBackend(const LocalAsrProvider& provider,
                                                         LocalAsrModelContext context,
                                                         std::string* error) {
  context.asr_config.vad_enabled = false;
  context.asr_config.vad_model_path.clear();

  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<SherpaStreamingBackend>(context.info, std::move(context.asr_config),
                                                  provider.id);
}

} // namespace vinput::daemon::asr
