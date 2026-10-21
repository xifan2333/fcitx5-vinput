#include "daemon/asr/backends/sherpa_streaming_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <glaze/json/json_t.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/asr_config.h"
#include "daemon/asr/hotword_utils.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/sherpa_json_helpers.h"

namespace vinput::daemon::asr {

namespace {

int ChooseNumThreads(const glz::json_t& model_cfg, int runtime_default) {
  if (runtime_default > 0) {
    return runtime_default;
  }
  return JsonInt(model_cfg, "num_threads", 1);
}

class SherpaStreamingSession : public RecognitionSession {
public:
  SherpaStreamingSession(const sherpa_onnx::cxx::OnlineRecognizer* recognizer,
                         sherpa_onnx::cxx::OnlineStream stream)
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

    if (!stream_.has_value() || stream_->Get() == nullptr || recognizer_ == nullptr ||
        recognizer_->Get() == nullptr) {
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

    auto& stream = *stream_;
    UpdateAudioStats(samples);

    stream.AcceptWaveform(16000, samples.data(), static_cast<int32_t>(samples.size()));
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

    if (!stream_.has_value() || stream_->Get() == nullptr || recognizer_ == nullptr ||
        recognizer_->Get() == nullptr) {
      if (error != nullptr) {
        *error = "Streaming recognizer is not initialized.";
      }
      return false;
    }

    auto& stream = *stream_;
    finished_ = true;
    stream.InputFinished();
    DecodeAvailable();

    std::string text = GetCurrentText();
    fprintf(stderr, "vinput: streaming finish current result bytes=%zu\n", text.size());
    if (!text.empty()) {
      if (text != last_partial_text_) {
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
        last_partial_text_ = text;
      }
      events_.push_back({RecognitionEventKind::FinalText, std::move(text), {}});
      fprintf(stderr, "vinput: streaming queued final result\n");
    } else {
      const double rms =
          total_samples_ > 0 ? std::sqrt(sum_squares_ / static_cast<double>(total_samples_)) : 0.0;
      fprintf(stderr,
              "vinput: streaming finish produced empty text samples=%zu "
              "peak=%.4f rms=%.4f\n",
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
    if (!stream_.has_value() || stream_->Get() == nullptr || recognizer_ == nullptr ||
        recognizer_->Get() == nullptr) {
      return;
    }

    auto& stream = *stream_;
    int decode_iterations = 0;
    while (recognizer_->IsReady(&stream)) {
      ++decode_iterations;
      recognizer_->Decode(&stream);
      const std::string text = GetCurrentText();
      if (!text.empty() && text != last_partial_text_) {
        last_partial_text_ = text;
        fprintf(stderr, "vinput: streaming partial result bytes=%zu decode_iterations=%d\n",
                text.size(), decode_iterations);
        events_.push_back({RecognitionEventKind::PartialText, text, {}});
      }
    }
    if (decode_iterations > 0) {
      fprintf(stderr, "vinput: streaming decode loop completed iterations=%d\n", decode_iterations);
    }
  }

  [[nodiscard]] std::string GetCurrentText() const {
    if (!stream_.has_value() || stream_->Get() == nullptr || recognizer_ == nullptr ||
        recognizer_->Get() == nullptr) {
      return {};
    }

    const auto& stream = *stream_;
    const auto result = recognizer_->GetResult(&stream);
    std::string text = std::string(vinput::str::Trim(result.text));
    fprintf(stderr, "vinput: streaming current result bytes=%zu token_count=%zu\n", text.size(),
            result.tokens.size());
    return text;
  }

  void Reset() {
    stream_.reset();
    recognizer_ = nullptr;
  }

  const sherpa_onnx::cxx::OnlineRecognizer* recognizer_ = nullptr;
  std::optional<sherpa_onnx::cxx::OnlineStream> stream_;
  bool finished_ = false;
  std::size_t total_samples_ = 0;
  double peak_abs_ = 0.0;
  double sum_squares_ = 0.0;
  std::string last_partial_text_;
  std::vector<RecognitionEvent> events_;
};

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
    if (!InitializeRecognizerLocked(error) || !recognizer_.has_value() ||
        recognizer_->Get() == nullptr) {
      return nullptr;
    }

    auto stream = recognizer_->CreateStream();
    if (stream.Get() == nullptr) {
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
    if (recognizer_.has_value() && recognizer_->Get() != nullptr) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    sherpa_onnx::cxx::OnlineRecognizerConfig config;
    const auto& recognizer_cfg = model_info_.recognizer_config;
    const auto& model_cfg = model_info_.model_config;
    const auto& feat_cfg = vinput::json::Object(recognizer_cfg, "feat_config");
    config.feat_config.sample_rate = JsonInt(feat_cfg, "sample_rate", 16000);
    config.feat_config.feature_dim = JsonInt(feat_cfg, "feature_dim", 80);

    const std::string tokens_path = model_info_.File("tokens");
    const std::string f_model = model_info_.File("model");
    const std::string f_encoder = model_info_.File("encoder");
    const std::string f_decoder = model_info_.File("decoder");
    const std::string f_joiner = model_info_.File("joiner");
    const std::string f_bpe_vocab = model_info_.File("bpe_vocab");
    const std::string f_rule_fsts = model_info_.File("rule_fsts");
    const std::string f_rule_fars = model_info_.File("rule_fars");
    const std::string f_hotwords_file = model_info_.File("hotwords_file");
    const std::string f_graph = model_info_.File("graph");
    const std::string f_hr_lexicon = model_info_.File("hr_lexicon");
    const std::string f_hr_rule_fsts = model_info_.File("hr_rule_fsts");
    const std::string explicit_model_type = JsonString(model_cfg, "model_type");
    const std::string modeling_unit = JsonString(model_cfg, "modeling_unit");
    const std::string tokens_buf = JsonString(model_cfg, "tokens_buf");
    const std::string hotwords_buf = JsonString(recognizer_cfg, "hotwords_buf");
    std::string selected_hotwords_file;
    std::string normalized_transducer_hotwords;
    bool use_hotwords_buf = false;

    const bool provider_hotwords_configured =
        model_info_.supports_hotwords && !asr_config_.hotwords_file.empty();
    if (provider_hotwords_configured && !IsTransducerHotwordFamily(model_info_.family)) {
      if (error != nullptr) {
        *error = "model family '" + model_info_.family +
                 "' does not expose streaming ASR hotwords in sherpa-onnx";
      }
      return false;
    }
    if (model_info_.supports_hotwords && IsTransducerHotwordFamily(model_info_.family)) {
      selected_hotwords_file =
          !asr_config_.hotwords_file.empty() ? asr_config_.hotwords_file : f_hotwords_file;
      bool hotwords_active = false;
      if (!selected_hotwords_file.empty()) {
        if (!LoadTransducerHotwords(selected_hotwords_file, &normalized_transducer_hotwords,
                                    error)) {
          return false;
        }
        hotwords_active = !normalized_transducer_hotwords.empty();
      } else if (!hotwords_buf.empty()) {
        use_hotwords_buf = true;
        hotwords_active = true;
      }
      if (hotwords_active && !ValidateTransducerHotwordAssets(model_info_, error)) {
        return false;
      }
    }

    config.model_config.tokens = tokens_path;
    config.model_config.num_threads = ChooseNumThreads(model_cfg, asr_config_.thread_num);
    config.model_config.provider = JsonString(model_cfg, "provider", "cpu");
    config.decoding_method = JsonString(recognizer_cfg, "decoding_method", "greedy_search");
    config.max_active_paths = JsonInt(recognizer_cfg, "max_active_paths", 4);
    config.blank_penalty = JsonFloat(recognizer_cfg, "blank_penalty", 0.0F);
    config.enable_endpoint = JsonBool(recognizer_cfg, "enable_endpoint", true);
    config.rule1_min_trailing_silence =
        JsonFloat(recognizer_cfg, "rule1_min_trailing_silence", 2.4F);
    config.rule2_min_trailing_silence =
        JsonFloat(recognizer_cfg, "rule2_min_trailing_silence", 1.2F);
    config.rule3_min_utterance_length =
        JsonFloat(recognizer_cfg, "rule3_min_utterance_length", 20.0F);
    config.hotwords_score = JsonFloat(recognizer_cfg, "hotwords_score", 1.5F);
    if (!explicit_model_type.empty()) {
      config.model_config.model_type = explicit_model_type;
    }
    if (!modeling_unit.empty()) {
      config.model_config.modeling_unit = modeling_unit;
    }
    if (!f_bpe_vocab.empty()) {
      config.model_config.bpe_vocab = f_bpe_vocab;
    }
    if (!tokens_buf.empty()) {
      config.model_config.tokens_buf = tokens_buf;
    }
    if (!f_rule_fsts.empty()) {
      config.rule_fsts = f_rule_fsts;
    }
    if (!f_rule_fars.empty()) {
      config.rule_fars = f_rule_fars;
    }
    if (!f_graph.empty()) {
      config.ctc_fst_decoder_config.graph = f_graph;
      config.ctc_fst_decoder_config.max_active = JsonInt(
          vinput::json::Object(recognizer_cfg, "ctc_fst_decoder_config"), "max_active", 3000);
    }
    if (!normalized_transducer_hotwords.empty()) {
      config.hotwords_buf = normalized_transducer_hotwords;
      config.decoding_method = "modified_beam_search";
    } else if (use_hotwords_buf) {
      config.hotwords_buf = hotwords_buf;
      config.decoding_method = "modified_beam_search";
    }
    if (!f_hr_lexicon.empty()) {
      config.hr.lexicon = f_hr_lexicon;
    }
    if (!f_hr_rule_fsts.empty()) {
      config.hr.rule_fsts = f_hr_rule_fsts;
    }

    if (model_info_.family == "transducer") {
      config.model_config.transducer.encoder = f_encoder;
      config.model_config.transducer.decoder = f_decoder;
      config.model_config.transducer.joiner = f_joiner;
    } else if (model_info_.family == "zipformer2_ctc") {
      config.model_config.zipformer2_ctc.model = f_model;
    } else if (model_info_.family == "paraformer") {
      config.model_config.paraformer.encoder = f_encoder;
      config.model_config.paraformer.decoder = f_decoder;
    } else if (model_info_.family == "nemo_ctc") {
      config.model_config.nemo_ctc.model = f_model;
    } else if (model_info_.family == "t_one_ctc") {
      config.model_config.t_one_ctc.model = f_model;
    } else {
      if (error != nullptr) {
        *error = "Unsupported sherpa-streaming model family '" + model_info_.family + "'";
      }
      return false;
    }

    auto recognizer = sherpa_onnx::cxx::OnlineRecognizer::Create(config);
    if (recognizer.Get() == nullptr) {
      if (error != nullptr) {
        *error =
            "failed to create sherpa-streaming recognizer for family '" + model_info_.family + "'";
      }
      return false;
    }

    recognizer_ = std::move(recognizer);

    fprintf(stderr,
            "vinput: sherpa-onnx streaming ASR initialized successfully "
            "(family: %s, lang: %s, threads: %d)\n",
            model_info_.family.c_str(), asr_config_.language.c_str(),
            config.model_config.num_threads);

    WarmupRecognizerLocked();

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void WarmupRecognizerLocked() {
    if (!recognizer_.has_value() || recognizer_->Get() == nullptr) {
      return;
    }

    auto stream = recognizer_->CreateStream();
    if (stream.Get() == nullptr) {
      fprintf(stderr, "vinput: streaming warmup skipped (stream create failed)\n");
      return;
    }

    std::vector<float> silence(16000 / 5, 0.0F);
    stream.AcceptWaveform(16000, silence.data(), static_cast<int32_t>(silence.size()));
    while (recognizer_->IsReady(&stream)) {
      recognizer_->Decode(&stream);
    }
    stream.InputFinished();
    while (recognizer_->IsReady(&stream)) {
      recognizer_->Decode(&stream);
    }

    fprintf(stderr, "vinput: streaming recognizer warmup completed\n");
  }

  ModelInfo model_info_;
  AsrConfig asr_config_;
  std::string provider_id_;
  std::mutex recognizer_mutex_;
  std::optional<sherpa_onnx::cxx::OnlineRecognizer> recognizer_;
};

} // namespace

std::unique_ptr<AsrBackend> CreateSherpaStreamingBackend(const CoreConfig& config,
                                                         const LocalAsrProvider& provider,
                                                         std::string* error) {
  if (provider.model.empty()) {
    if (error != nullptr) {
      *error = vinput::str::FmtStr(_("Local ASR model configuration is missing for provider '%s'."),
                                   provider.id);
    }
    return nullptr;
  }

  ModelManager model_mgr(ResolveModelBaseDir(config).string(), provider.model);
  std::string model_error;
  if (!model_mgr.EnsureModels(&model_error)) {
    if (error != nullptr) {
      *error = "Local ASR model check failed for provider '" + provider.id + "'";
      if (!model_error.empty()) {
        *error += ": " + model_error;
      } else {
        *error += ".";
      }
    }
    return nullptr;
  }

  AsrConfig asr_config;
  const ModelInfo model_info = model_mgr.GetModelInfo();
  asr_config.language = model_info.RuntimeLanguageHint();
  asr_config.hotwords_file = provider.hotwordsFile;
  asr_config.vad_enabled = false;
  asr_config.vad_model_path.clear();

  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<SherpaStreamingBackend>(model_info, std::move(asr_config), provider.id);
}

} // namespace vinput::daemon::asr
