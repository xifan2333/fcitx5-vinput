#include "daemon/asr/backends/sherpa_offline_backend.h"

#include "common/i18n.h"
#include "common/utils/string_utils.h"
#include "daemon/asr/sherpa_json_helpers.h"
#include "daemon/asr/vad_trimmer.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <cstdio>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace vinput::daemon::asr {

namespace {

constexpr std::size_t kMinSamplesForInference = 8000; // 0.5 s @ 16 kHz

// ---------------------------------------------------------------------------
// Build a SherpaOnnxOfflineRecognizerConfig from ModelInfo + AsrConfig.
// Returns nullptr on failure (sets *error).
// ---------------------------------------------------------------------------
const SherpaOnnxOfflineRecognizer *
CreateOfflineRecognizer(const ModelInfo &info, const AsrConfig &asr_config,
                        std::string *error) {
  SherpaOnnxOfflineRecognizerConfig config = {};
  const auto &recognizer_cfg = info.recognizer_config;
  const auto &model_cfg = info.model_config;
  const auto feat_cfg =
      recognizer_cfg.contains("feat_config") &&
              recognizer_cfg["feat_config"].is_object()
          ? recognizer_cfg["feat_config"]
          : nlohmann::json::object();
  const auto lm_cfg =
      recognizer_cfg.contains("lm_config") &&
              recognizer_cfg["lm_config"].is_object()
          ? recognizer_cfg["lm_config"]
          : nlohmann::json::object();
  config.feat_config.sample_rate = JsonInt(feat_cfg, "sample_rate", 16000);
  config.feat_config.feature_dim = JsonInt(feat_cfg, "feature_dim", 80);

  const std::string p_decoding_method =
      JsonString(recognizer_cfg, "decoding_method", "greedy_search");
  config.decoding_method = p_decoding_method.c_str();
  config.max_active_paths = JsonInt(recognizer_cfg, "max_active_paths", 4);
  config.blank_penalty = JsonFloat(recognizer_cfg, "blank_penalty", 0.0f);

  const std::string tokens_path = info.File("tokens");
  const std::string f_model = info.File("model");
  const std::string f_encoder = info.File("encoder");
  const std::string f_decoder = info.File("decoder");
  const std::string f_joiner = info.File("joiner");
  const std::string f_preprocessor = info.File("preprocessor");
  const std::string f_uncached_decoder = info.File("uncached_decoder");
  const std::string f_cached_decoder = info.File("cached_decoder");
  const std::string f_merged_decoder = info.File("merged_decoder");
  const std::string f_encoder_adapter = info.File("encoder_adapter");
  const std::string f_llm = info.File("llm");
  const std::string f_embedding = info.File("embedding");
  const std::string f_tokenizer = info.File("tokenizer");
  const std::string f_conv_frontend = info.File("conv_frontend");
  const std::string f_lm = info.File("lm");
  const std::string f_hotwords_file = info.File("hotwords_file");
  const std::string f_bpe_vocab = info.File("bpe_vocab");
  const std::string f_rule_fsts = info.File("rule_fsts");
  const std::string f_rule_fars = info.File("rule_fars");
  const std::string p_language = asr_config.language;
  const std::string p_modeling_unit =
      JsonString(model_cfg, "modeling_unit", "cjkchar");
  std::string cfg_language;
  std::string cfg_task;
  std::string cfg_src_lang;
  std::string cfg_tgt_lang;
  std::string cfg_system_prompt;
  std::string cfg_user_prompt;
  std::string cfg_hotwords;
  std::string cfg_telespeech_path;

  if (!tokens_path.empty()) {
    config.model_config.tokens = tokens_path.c_str();
  }
  config.model_config.num_threads =
      JsonInt(model_cfg, "num_threads", asr_config.thread_num);
  const std::string provider = JsonString(model_cfg, "provider", "cpu");
  config.model_config.provider = provider.c_str();
  const std::string explicit_model_type = JsonString(model_cfg, "model_type");
  if (!explicit_model_type.empty()) {
    config.model_config.model_type = explicit_model_type.c_str();
  }

  if (!f_bpe_vocab.empty()) {
    config.model_config.bpe_vocab = f_bpe_vocab.c_str();
  }
  if (!p_modeling_unit.empty()) {
    config.model_config.modeling_unit = p_modeling_unit.c_str();
  }

  if (!f_lm.empty()) {
    config.lm_config.model = f_lm.c_str();
    config.lm_config.scale = JsonFloat(lm_cfg, "scale", 0.5f);
  }

  const auto &family = info.family;

  if (info.supports_hotwords) {
    if (!asr_config.hotwords_file.empty()) {
      config.hotwords_file = asr_config.hotwords_file.c_str();
      config.decoding_method = "modified_beam_search";
    } else if (!f_hotwords_file.empty()) {
      config.hotwords_file = f_hotwords_file.c_str();
      config.decoding_method = "modified_beam_search";
    }
  }

  if (!f_rule_fsts.empty()) {
    config.rule_fsts = f_rule_fsts.c_str();
  }
  if (!f_rule_fars.empty()) {
    config.rule_fars = f_rule_fars.c_str();
  }

  const auto &family_cfg =
      model_cfg.contains(family) && model_cfg[family].is_object()
          ? model_cfg[family]
          : nlohmann::json::object();

  const auto handlers = std::unordered_map<std::string, std::function<void()>>{
      {"paraformer",
       [&] {
         config.model_config.paraformer.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "paraformer";
       }},
      {"sense_voice",
       [&] {
         config.model_config.sense_voice.model = f_model.c_str();
         cfg_language = JsonString(family_cfg, "language", p_language);
         config.model_config.sense_voice.language = cfg_language.c_str();
         config.model_config.sense_voice.use_itn =
             JsonBool(family_cfg, "use_itn") ? 1 : 0;
         if (!config.model_config.model_type)
           config.model_config.model_type = "sense_voice";
       }},
      {"whisper",
       [&] {
         config.model_config.whisper.encoder = f_encoder.c_str();
         config.model_config.whisper.decoder = f_decoder.c_str();
         cfg_language = JsonString(family_cfg, "language", p_language);
         cfg_task = JsonString(family_cfg, "task", "transcribe");
         config.model_config.whisper.language = cfg_language.c_str();
         config.model_config.whisper.task = cfg_task.c_str();
         config.model_config.whisper.tail_paddings =
             JsonInt(family_cfg, "tail_paddings", -1);
         config.model_config.whisper.enable_token_timestamps =
             JsonBool(family_cfg, "enable_token_timestamps") ? 1 : 0;
         config.model_config.whisper.enable_segment_timestamps =
             JsonBool(family_cfg, "enable_segment_timestamps") ? 1 : 0;
         if (!config.model_config.model_type)
           config.model_config.model_type = "whisper";
       }},
      {"moonshine",
       [&] {
         config.model_config.moonshine.preprocessor = f_preprocessor.c_str();
         config.model_config.moonshine.encoder = f_encoder.c_str();
         config.model_config.moonshine.uncached_decoder =
             f_uncached_decoder.c_str();
         config.model_config.moonshine.cached_decoder =
             f_cached_decoder.c_str();
         if (!f_merged_decoder.empty())
           config.model_config.moonshine.merged_decoder =
               f_merged_decoder.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "moonshine";
       }},
      {"transducer",
       [&] {
         config.model_config.transducer.encoder = f_encoder.c_str();
         config.model_config.transducer.decoder = f_decoder.c_str();
         config.model_config.transducer.joiner = f_joiner.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "transducer";
       }},
      {"nemo_transducer",
       [&] {
         config.model_config.transducer.encoder = f_encoder.c_str();
         config.model_config.transducer.decoder = f_decoder.c_str();
         config.model_config.transducer.joiner = f_joiner.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "nemo_transducer";
       }},
      {"zipformer_ctc",
       [&] {
         config.model_config.zipformer_ctc.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "zipformer_ctc";
       }},
      {"fire_red_asr",
       [&] {
         config.model_config.fire_red_asr.encoder = f_encoder.c_str();
         config.model_config.fire_red_asr.decoder = f_decoder.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "fire_red_asr";
       }},
      {"fire_red_asr_ctc",
       [&] {
         config.model_config.fire_red_asr_ctc.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "fire_red_asr_ctc";
       }},
      {"dolphin",
       [&] {
         config.model_config.dolphin.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "dolphin";
       }},
      {"nemo_ctc",
       [&] {
         config.model_config.nemo_ctc.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "nemo_ctc";
       }},
      {"wenet_ctc",
       [&] {
         config.model_config.wenet_ctc.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "wenet_ctc";
       }},
      {"tdnn",
       [&] {
         config.model_config.tdnn.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "tdnn";
       }},
      {"omnilingual",
       [&] {
         config.model_config.omnilingual.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "omnilingual";
       }},
      {"medasr",
       [&] {
         config.model_config.medasr.model = f_model.c_str();
         if (!config.model_config.model_type)
           config.model_config.model_type = "medasr";
       }},
      {"canary",
       [&] {
         config.model_config.canary.encoder = f_encoder.c_str();
         config.model_config.canary.decoder = f_decoder.c_str();
         cfg_src_lang = JsonString(family_cfg, "src_lang", p_language);
         cfg_tgt_lang =
             JsonString(family_cfg, "tgt_lang", asr_config.language);
         config.model_config.canary.src_lang = cfg_src_lang.c_str();
         config.model_config.canary.tgt_lang = cfg_tgt_lang.c_str();
         config.model_config.canary.use_pnc =
             JsonBool(family_cfg, "use_pnc") ? 1 : 0;
         if (!config.model_config.model_type)
           config.model_config.model_type = "canary";
       }},
      {"funasr_nano",
       [&] {
         config.model_config.funasr_nano.encoder_adaptor =
             f_encoder_adapter.c_str();
         config.model_config.funasr_nano.llm = f_llm.c_str();
         config.model_config.funasr_nano.embedding = f_embedding.c_str();
         config.model_config.funasr_nano.tokenizer = f_tokenizer.c_str();
         cfg_language = JsonString(family_cfg, "language", p_language);
         cfg_system_prompt = JsonString(family_cfg, "system_prompt");
         cfg_user_prompt = JsonString(family_cfg, "user_prompt");
         cfg_hotwords = JsonString(family_cfg, "hotwords");
         config.model_config.funasr_nano.language = cfg_language.c_str();
         config.model_config.funasr_nano.itn =
             JsonBool(family_cfg, "itn") ? 1 : 0;
         if (!cfg_system_prompt.empty())
           config.model_config.funasr_nano.system_prompt =
               cfg_system_prompt.c_str();
         if (!cfg_user_prompt.empty())
           config.model_config.funasr_nano.user_prompt =
               cfg_user_prompt.c_str();
         if (!cfg_hotwords.empty())
           config.model_config.funasr_nano.hotwords = cfg_hotwords.c_str();
         config.model_config.funasr_nano.max_new_tokens =
             JsonInt(family_cfg, "max_new_tokens", 1024);
         config.model_config.funasr_nano.temperature =
             JsonFloat(family_cfg, "temperature", 1.0f);
         config.model_config.funasr_nano.top_p =
             JsonFloat(family_cfg, "top_p", 0.9f);
         config.model_config.funasr_nano.seed =
             JsonInt(family_cfg, "seed", 0);
         if (!config.model_config.model_type)
           config.model_config.model_type = "funasr_nano";
       }},
      {"qwen3_asr",
       [&] {
         config.model_config.qwen3_asr.conv_frontend =
             f_conv_frontend.c_str();
         config.model_config.qwen3_asr.encoder = f_encoder.c_str();
         config.model_config.qwen3_asr.decoder = f_decoder.c_str();
         config.model_config.qwen3_asr.tokenizer = f_tokenizer.c_str();
         config.model_config.qwen3_asr.max_total_len =
             JsonInt(family_cfg, "max_total_len", 4096);
         config.model_config.qwen3_asr.max_new_tokens =
             JsonInt(family_cfg, "max_new_tokens", 1024);
         config.model_config.qwen3_asr.temperature =
             JsonFloat(family_cfg, "temperature", 1.0f);
         config.model_config.qwen3_asr.top_p =
             JsonFloat(family_cfg, "top_p", 0.9f);
         config.model_config.qwen3_asr.seed = JsonInt(family_cfg, "seed", 0);
         if (!config.model_config.model_type)
           config.model_config.model_type = "qwen3_asr";
       }}};

  if (family == "telespeech_ctc") {
    cfg_telespeech_path = info.File("telespeech_ctc");
    config.model_config.telespeech_ctc = cfg_telespeech_path.c_str();
    if (!config.model_config.model_type)
      config.model_config.model_type = "telespeech_ctc";
  } else {
    const auto it = handlers.find(family);
    if (it == handlers.end()) {
      if (error)
        *error = "unsupported model family '" + family + "'";
      return nullptr;
    }
    it->second();
  }

  auto *recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
  if (!recognizer) {
    if (error)
      *error =
          "failed to create sherpa-onnx recognizer for family '" + family + "'";
    return nullptr;
  }

  fprintf(stderr,
          "vinput: sherpa-onnx offline ASR initialized (family: %s, lang: %s)\n",
          family.c_str(), asr_config.language.c_str());
  return recognizer;
}

// ---------------------------------------------------------------------------
// Run inference on a recognizer with pre-processed float samples.
// ---------------------------------------------------------------------------
std::string InferOffline(const SherpaOnnxOfflineRecognizer *recognizer,
                         const std::vector<float> &samples) {
  const SherpaOnnxOfflineStream *stream =
      SherpaOnnxCreateOfflineStream(recognizer);
  if (!stream)
    return {};

  SherpaOnnxAcceptWaveformOffline(stream, 16000, samples.data(),
                                  static_cast<int32_t>(samples.size()));
  SherpaOnnxDecodeOfflineStream(recognizer, stream);

  const SherpaOnnxOfflineRecognizerResult *result =
      SherpaOnnxGetOfflineStreamResult(stream);
  std::string text;
  if (result && result->text)
    text = result->text;
  if (result)
    SherpaOnnxDestroyOfflineRecognizerResult(result);
  SherpaOnnxDestroyOfflineStream(stream);
  return text;
}

// ---------------------------------------------------------------------------
// Session: buffers audio, runs inference on Finish().
// ---------------------------------------------------------------------------
class SherpaOfflineSession : public RecognitionSession {
public:
  SherpaOfflineSession(const SherpaOnnxOfflineRecognizer *recognizer,
                       bool vad_available, VadTrimmer *vad)
      : recognizer_(recognizer),
        vad_available_(vad_available),
        vad_(vad) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string *error) override {
    if (finished_) {
      if (error)
        *error = "Recognition session already finished.";
      return false;
    }
    pcm_.insert(pcm_.end(), pcm.begin(), pcm.end());
    if (error)
      error->clear();
    return true;
  }

  bool Finish(std::string *error) override {
    if (finished_) {
      if (error)
        error->clear();
      return true;
    }
    finished_ = true;

    if (pcm_.size() < kMinSamplesForInference) {
      fprintf(stderr,
              "vinput: skipping ASR for short audio: %zu samples (%.1f ms)\n",
              pcm_.size(),
              static_cast<double>(pcm_.size()) * 1000.0 / 16000.0);
      events_.push_back({RecognitionEventKind::Completed, {}, {}});
      if (error)
        error->clear();
      return true;
    }

    // Convert int16 → float [-1, 1]
    std::vector<float> samples(pcm_.size());
    for (size_t i = 0; i < pcm_.size(); ++i)
      samples[i] = static_cast<float>(pcm_[i]) / 32768.0f;

    if (vad_available_ && vad_) {
      samples = vad_->Trim(samples, 16000);
      if (samples.size() < kMinSamplesForInference) {
        fprintf(stderr, "vinput: audio too short after VAD trim, skipping\n");
        events_.push_back({RecognitionEventKind::Completed, {}, {}});
        if (error)
          error->clear();
        return true;
      }
    }

    std::string text = InferOffline(recognizer_, samples);
    if (!text.empty())
      events_.push_back({RecognitionEventKind::FinalText, std::move(text), {}});
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    if (error)
      error->clear();
    return true;
  }

  void Cancel() override {
    finished_ = true;
    pcm_.clear();
    events_.clear();
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  const SherpaOnnxOfflineRecognizer *recognizer_;
  bool vad_available_;
  VadTrimmer *vad_;
  bool finished_ = false;
  std::vector<int16_t> pcm_;
  std::vector<RecognitionEvent> events_;
};

// ---------------------------------------------------------------------------
// Backend: owns recognizer (lazy init) and shared VAD.
// ---------------------------------------------------------------------------
class SherpaOfflineBackend : public AsrBackend {
public:
  SherpaOfflineBackend(ModelInfo model_info, AsrConfig asr_config,
                       std::string provider_id)
      : model_info_(std::move(model_info)),
        asr_config_(std::move(asr_config)),
        provider_id_(std::move(provider_id)) {}

  ~SherpaOfflineBackend() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recognizer_) {
      SherpaOnnxDestroyOfflineRecognizer(recognizer_);
      recognizer_ = nullptr;
    }
  }

  BackendDescriptor Describe() const override {
    BackendDescriptor descriptor;
    descriptor.provider_id = provider_id_;
    descriptor.provider_type = vinput::asr::kLocalProviderType;
    descriptor.backend_id = "sherpa-offline";
    descriptor.capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
    descriptor.capabilities.supports_hotwords = model_info_.supports_hotwords;
    return descriptor;
  }

  std::unique_ptr<RecognitionSession>
  CreateSession(std::string *error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!InitializeLocked(error))
      return nullptr;
    if (error)
      error->clear();
    return std::make_unique<SherpaOfflineSession>(
        recognizer_, vad_.Available(), &vad_);
  }

private:
  bool InitializeLocked(std::string *error) {
    if (recognizer_) {
      if (error)
        error->clear();
      return true;
    }

    recognizer_ = CreateOfflineRecognizer(model_info_, asr_config_, error);
    if (!recognizer_)
      return false;

    if (asr_config_.vad_enabled && !asr_config_.vad_model_path.empty()) {
      std::string vad_error;
      if (!vad_.Init(asr_config_.vad_model_path, 16000, &vad_error)) {
        fprintf(stderr, "vinput: %s, continuing without VAD\n",
                vad_error.empty() ? "VAD model not available"
                                  : vad_error.c_str());
      }
    }

    if (error)
      error->clear();
    return true;
  }

  ModelInfo model_info_;
  AsrConfig asr_config_;
  std::string provider_id_;
  std::mutex mutex_;
  const SherpaOnnxOfflineRecognizer *recognizer_ = nullptr;
  VadTrimmer vad_;
};

} // namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<AsrBackend>
CreateSherpaOfflineBackend(const CoreConfig &config,
                           const LocalAsrProvider &provider,
                           std::string *error) {
  if (provider.model.empty()) {
    if (error) {
      *error = vinput::str::FmtStr(
          _("Local ASR model configuration is missing for provider '%s'."),
          provider.id);
    }
    return nullptr;
  }

  ModelManager model_mgr(ResolveModelBaseDir(config).string(), provider.model);
  std::string model_error;
  if (!model_mgr.EnsureModels(&model_error)) {
    if (error) {
      *error =
          "Local ASR model check failed for provider '" + provider.id + "'";
      if (!model_error.empty())
        *error += ": " + model_error;
      else
        *error += ".";
    }
    return nullptr;
  }

  AsrConfig asr_config;
  const ModelInfo model_info = model_mgr.GetModelInfo();
  asr_config.language = model_info.RuntimeLanguageHint();
  asr_config.hotwords_file = provider.hotwordsFile;
  asr_config.vad_enabled = config.asr.vad.enabled;
  asr_config.vad_model_path = VINPUT_VAD_MODEL_PATH;

  if (error)
    error->clear();
  return std::make_unique<SherpaOfflineBackend>(model_info,
                                                std::move(asr_config),
                                                provider.id);
}

} // namespace vinput::daemon::asr
