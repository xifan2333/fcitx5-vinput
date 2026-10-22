#include "daemon/asr/backends/sherpa_offline_backend.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/utils/file_utils.h"

#include "daemon/asr/asr_config.h"
#include "daemon/asr/hotword_utils.h"
#include "daemon/asr/local_asr_model_loader.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/sherpa_json_helpers.h"
#include "daemon/asr/vad_trimmer.h"

namespace vinput::daemon::asr {

namespace {

constexpr std::size_t kMinSamplesForInference = 8000; // 0.5 s @ 16 kHz

bool WriteTemporaryHotwordFile(std::string_view content, std::string* path, std::string* error) {
  std::error_code temp_error;
  const auto temp_dir = std::filesystem::temp_directory_path(temp_error);
  if (temp_error) {
    if (error != nullptr) {
      *error = "failed to locate temporary directory: " + temp_error.message();
    }
    return false;
  }
  const auto temp_file =
      temp_dir /
      ("vinput-hotwords-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
  if (auto res = vinput::file::AtomicWriteTextFile(temp_file, content); !res) {
    if (error != nullptr) {
      *error = res.error();
    }
    return false;
  }
  *path = temp_file.string();
  return true;
}

// ---------------------------------------------------------------------------
// Build a sherpa_onnx::cxx::OfflineRecognizer from ModelInfo + AsrConfig.
// Returns std::nullopt on failure (sets *error).
// ---------------------------------------------------------------------------
std::optional<sherpa_onnx::cxx::OfflineRecognizer>
CreateOfflineRecognizer(const ModelInfo& info, const AsrConfig& asr_config, std::string* error) {
  sherpa_onnx::cxx::OfflineRecognizerConfig config;
  const auto& recognizer_cfg = info.recognizer_config;
  const auto& model_cfg = info.model_config;
  const auto& feat_cfg = vinput::json::Object(recognizer_cfg, "feat_config");
  const auto& lm_cfg = vinput::json::Object(recognizer_cfg, "lm_config");
  config.feat_config.sample_rate = JsonInt(feat_cfg, "sample_rate", 16000);
  config.feat_config.feature_dim = JsonInt(feat_cfg, "feature_dim", 80);

  config.decoding_method = JsonString(recognizer_cfg, "decoding_method", "greedy_search");
  config.max_active_paths = JsonInt(recognizer_cfg, "max_active_paths", 4);
  config.hotwords_score = JsonFloat(recognizer_cfg, "hotwords_score", 1.5F);
  config.blank_penalty = JsonFloat(recognizer_cfg, "blank_penalty", 0.0F);

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
  const std::string p_modeling_unit = JsonString(model_cfg, "modeling_unit", "cjkchar");

  std::string selected_hotwords_file;
  std::string normalized_transducer_hotwords;
  std::string provider_prompt_hotwords;

  const bool provider_hotwords_configured =
      info.supports_hotwords && !asr_config.hotwords_file.empty();
  if (provider_hotwords_configured && IsPromptHotwordFamily(info.family)) {
    if (!LoadPromptHotwordsCsv(asr_config.hotwords_file, &provider_prompt_hotwords, error)) {
      return std::nullopt;
    }
  } else if (provider_hotwords_configured && !IsTransducerHotwordFamily(info.family)) {
    if (error != nullptr) {
      *error =
          "model family '" + info.family + "' does not expose an ASR hotword input in sherpa-onnx";
    }
    return std::nullopt;
  }

  if (info.supports_hotwords && IsTransducerHotwordFamily(info.family)) {
    selected_hotwords_file =
        !asr_config.hotwords_file.empty() ? asr_config.hotwords_file : f_hotwords_file;
    if (!selected_hotwords_file.empty()) {
      if (!LoadTransducerHotwords(selected_hotwords_file, &normalized_transducer_hotwords, error)) {
        return std::nullopt;
      }
      if (!normalized_transducer_hotwords.empty()) {
        if (!ValidateTransducerHotwordAssets(info, error)) {
          return std::nullopt;
        }
        config.decoding_method = "modified_beam_search";
      }
    }
  }

  if (!tokens_path.empty()) {
    config.model_config.tokens = tokens_path;
  }
  config.model_config.num_threads = JsonInt(model_cfg, "num_threads", asr_config.thread_num);
  config.model_config.provider = JsonString(model_cfg, "provider", "cpu");

  const std::string explicit_model_type = JsonString(model_cfg, "model_type");
  if (!explicit_model_type.empty()) {
    config.model_config.model_type = explicit_model_type;
  }

  if (!f_bpe_vocab.empty()) {
    config.model_config.bpe_vocab = f_bpe_vocab;
  }
  if (!p_modeling_unit.empty()) {
    config.model_config.modeling_unit = p_modeling_unit;
  }

  if (!f_lm.empty()) {
    config.lm_config.model = f_lm;
    config.lm_config.scale = JsonFloat(lm_cfg, "scale", 0.5F);
  }

  const auto& family = info.family;

  if (!f_rule_fsts.empty()) {
    config.rule_fsts = f_rule_fsts;
  }
  if (!f_rule_fars.empty()) {
    config.rule_fars = f_rule_fars;
  }

  const auto& family_cfg = vinput::json::Object(model_cfg, family);

  const auto handlers = std::unordered_map<std::string, std::function<void()>>{
      {"paraformer",
       [&] {
         config.model_config.paraformer.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "paraformer";
         }
       }},
      {"sense_voice",
       [&] {
         config.model_config.sense_voice.model = f_model;
         config.model_config.sense_voice.language = JsonString(family_cfg, "language", p_language);
         config.model_config.sense_voice.use_itn = JsonBool(family_cfg, "use_itn");
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "sense_voice";
         }
       }},
      {"whisper",
       [&] {
         config.model_config.whisper.encoder = f_encoder;
         config.model_config.whisper.decoder = f_decoder;
         config.model_config.whisper.language = JsonString(family_cfg, "language", p_language);
         config.model_config.whisper.task = JsonString(family_cfg, "task", "transcribe");
         config.model_config.whisper.tail_paddings = JsonInt(family_cfg, "tail_paddings", -1);
         config.model_config.whisper.enable_token_timestamps =
             JsonBool(family_cfg, "enable_token_timestamps");
         config.model_config.whisper.enable_segment_timestamps =
             JsonBool(family_cfg, "enable_segment_timestamps");
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "whisper";
         }
       }},
      {"moonshine",
       [&] {
         config.model_config.moonshine.preprocessor = f_preprocessor;
         config.model_config.moonshine.encoder = f_encoder;
         config.model_config.moonshine.uncached_decoder = f_uncached_decoder;
         config.model_config.moonshine.cached_decoder = f_cached_decoder;
         if (!f_merged_decoder.empty()) {
           config.model_config.moonshine.merged_decoder = f_merged_decoder;
         }
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "moonshine";
         }
       }},
      {"transducer",
       [&] {
         config.model_config.transducer.encoder = f_encoder;
         config.model_config.transducer.decoder = f_decoder;
         config.model_config.transducer.joiner = f_joiner;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "transducer";
         }
       }},
      {"nemo_transducer",
       [&] {
         config.model_config.transducer.encoder = f_encoder;
         config.model_config.transducer.decoder = f_decoder;
         config.model_config.transducer.joiner = f_joiner;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "nemo_transducer";
         }
       }},
      {"zipformer_ctc",
       [&] {
         config.model_config.zipformer_ctc.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "zipformer_ctc";
         }
       }},
      {"fire_red_asr",
       [&] {
         config.model_config.fire_red_asr.encoder = f_encoder;
         config.model_config.fire_red_asr.decoder = f_decoder;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "fire_red_asr";
         }
       }},
      {"fire_red_asr_ctc",
       [&] {
         config.model_config.fire_red_asr_ctc.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "fire_red_asr_ctc";
         }
       }},
      {"dolphin",
       [&] {
         config.model_config.dolphin.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "dolphin";
         }
       }},
      {"nemo_ctc",
       [&] {
         config.model_config.nemo_ctc.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "nemo_ctc";
         }
       }},
      {"wenet_ctc",
       [&] {
         config.model_config.wenet_ctc.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "wenet_ctc";
         }
       }},
      {"tdnn",
       [&] {
         config.model_config.tdnn.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "tdnn";
         }
       }},
      {"omnilingual",
       [&] {
         config.model_config.omnilingual.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "omnilingual";
         }
       }},
      {"medasr",
       [&] {
         config.model_config.medasr.model = f_model;
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "medasr";
         }
       }},
      {"canary",
       [&] {
         config.model_config.canary.encoder = f_encoder;
         config.model_config.canary.decoder = f_decoder;
         config.model_config.canary.src_lang = JsonString(family_cfg, "src_lang", p_language);
         config.model_config.canary.tgt_lang =
             JsonString(family_cfg, "tgt_lang", asr_config.language);
         config.model_config.canary.use_pnc = JsonBool(family_cfg, "use_pnc");
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "canary";
         }
       }},
      {"funasr_nano",
       [&] {
         config.model_config.funasr_nano.encoder_adaptor = f_encoder_adapter;
         config.model_config.funasr_nano.llm = f_llm;
         config.model_config.funasr_nano.embedding = f_embedding;
         config.model_config.funasr_nano.tokenizer = f_tokenizer;
         config.model_config.funasr_nano.language = JsonString(family_cfg, "language", p_language);
         config.model_config.funasr_nano.itn = JsonBool(family_cfg, "itn");
         const auto system_prompt = JsonString(family_cfg, "system_prompt");
         if (!system_prompt.empty()) {
           config.model_config.funasr_nano.system_prompt = system_prompt;
         }
         const auto user_prompt = JsonString(family_cfg, "user_prompt");
         if (!user_prompt.empty()) {
           config.model_config.funasr_nano.user_prompt = user_prompt;
         }
         const auto hotwords = provider_hotwords_configured ? provider_prompt_hotwords
                                                            : JsonString(family_cfg, "hotwords");
         if (!hotwords.empty()) {
           config.model_config.funasr_nano.hotwords = hotwords;
         }
         config.model_config.funasr_nano.max_new_tokens =
             JsonInt(family_cfg, "max_new_tokens", 1024);
         config.model_config.funasr_nano.temperature = JsonFloat(family_cfg, "temperature", 1.0F);
         config.model_config.funasr_nano.top_p = JsonFloat(family_cfg, "top_p", 0.9F);
         config.model_config.funasr_nano.seed = JsonInt(family_cfg, "seed", 0);
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "funasr_nano";
         }
       }},
      {"qwen3_asr", [&] {
         config.model_config.qwen3_asr.conv_frontend = f_conv_frontend;
         config.model_config.qwen3_asr.encoder = f_encoder;
         config.model_config.qwen3_asr.decoder = f_decoder;
         config.model_config.qwen3_asr.tokenizer = f_tokenizer;
         config.model_config.qwen3_asr.max_total_len = JsonInt(family_cfg, "max_total_len", 4096);
         config.model_config.qwen3_asr.max_new_tokens = JsonInt(family_cfg, "max_new_tokens", 1024);
         config.model_config.qwen3_asr.temperature = JsonFloat(family_cfg, "temperature", 1.0F);
         config.model_config.qwen3_asr.top_p = JsonFloat(family_cfg, "top_p", 0.9F);
         config.model_config.qwen3_asr.seed = JsonInt(family_cfg, "seed", 0);
         const auto qwen_hotwords = provider_hotwords_configured
                                        ? provider_prompt_hotwords
                                        : JsonString(family_cfg, "hotwords");
         if (!qwen_hotwords.empty()) {
           config.model_config.qwen3_asr.hotwords = qwen_hotwords;
         }
         if (config.model_config.model_type.empty()) {
           config.model_config.model_type = "qwen3_asr";
         }
       }}};

  if (family == "telespeech_ctc") {
    config.model_config.telespeech_ctc = info.File("telespeech_ctc");
    if (config.model_config.model_type.empty()) {
      config.model_config.model_type = "telespeech_ctc";
    }
  } else {
    const auto it = handlers.find(family);
    if (it == handlers.end()) {
      if (error != nullptr) {
        *error = "unsupported model family '" + family + "'";
      }
      return std::nullopt;
    }
    it->second();
  }

  std::string temporary_hotwords_file;
  if (!normalized_transducer_hotwords.empty()) {
    if (!WriteTemporaryHotwordFile(normalized_transducer_hotwords, &temporary_hotwords_file,
                                   error)) {
      return std::nullopt;
    }
    config.hotwords_file = temporary_hotwords_file;
  }

  auto recognizer = sherpa_onnx::cxx::OfflineRecognizer::Create(config);
  if (!temporary_hotwords_file.empty()) {
    std::error_code remove_error;
    std::filesystem::remove(temporary_hotwords_file, remove_error);
    if (remove_error) {
      fprintf(stderr, "vinput: failed to remove temporary hotwords file: %s\n",
              remove_error.message().c_str());
    }
  }
  if (recognizer.Get() == nullptr) {
    if (error != nullptr) {
      *error = "failed to create sherpa-onnx recognizer for family '" + family + "'";
    }
    return std::nullopt;
  }

  fprintf(stderr, "vinput: sherpa-onnx offline ASR initialized (family: %s, lang: %s)\n",
          family.c_str(), asr_config.language.c_str());
  return recognizer;
}

// ---------------------------------------------------------------------------
// Run inference on a recognizer with pre-processed float samples.
// ---------------------------------------------------------------------------
std::string InferOffline(const sherpa_onnx::cxx::OfflineRecognizer* recognizer,
                         const std::vector<float>& samples) {
  if (recognizer == nullptr || recognizer->Get() == nullptr) {
    return {};
  }

  auto stream = recognizer->CreateStream();
  if (stream.Get() == nullptr) {
    return {};
  }

  stream.AcceptWaveform(16000, samples.data(), static_cast<int32_t>(samples.size()));
  recognizer->Decode(&stream);

  const auto result = recognizer->GetResult(&stream);
  return result.text;
}

// ---------------------------------------------------------------------------
// Session: buffers audio, runs inference on Finish().
// ---------------------------------------------------------------------------
class SherpaOfflineSession : public RecognitionSession {
public:
  SherpaOfflineSession(const sherpa_onnx::cxx::OfflineRecognizer* recognizer, bool vad_available,
                       VadTrimmer* vad)
      : recognizer_(recognizer), vad_available_(vad_available), vad_(vad) {}

  using RecognitionSession::PushAudio;

  bool PushAudio(std::span<const float> samples, std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        *error = "Recognition session already finished.";
      }
      return false;
    }
    samples_.insert(samples_.end(), samples.begin(), samples.end());
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        *error = "Recognition session already finished.";
      }
      return false;
    }
    samples_.reserve(samples_.size() + pcm.size());
    for (const int16_t sample : pcm) {
      samples_.push_back(static_cast<float>(sample) / 32768.0F);
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    if (finished_) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    finished_ = true;

    if (samples_.size() < kMinSamplesForInference) {
      fprintf(stderr, "vinput: skipping ASR for short audio: %zu samples (%.1f ms)\n",
              samples_.size(), static_cast<double>(samples_.size()) * 1000.0 / 16000.0);
      events_.push_back({RecognitionEventKind::Completed, {}, {}});
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    if (vad_available_ && vad_ != nullptr) {
      samples_ = vad_->Trim(samples_, 16000);
      if (samples_.size() < kMinSamplesForInference) {
        fprintf(stderr, "vinput: audio too short after VAD trim, skipping\n");
        events_.push_back({RecognitionEventKind::Completed, {}, {}});
        if (error != nullptr) {
          error->clear();
        }
        return true;
      }
    }

    std::string text = InferOffline(recognizer_, samples_);
    if (!text.empty()) {
      events_.push_back({RecognitionEventKind::FinalText, std::move(text), {}});
    }
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void Cancel() override {
    finished_ = true;
    samples_.clear();
    events_.clear();
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  const sherpa_onnx::cxx::OfflineRecognizer* recognizer_;
  bool vad_available_;
  VadTrimmer* vad_;
  bool finished_ = false;
  std::vector<float> samples_;
  std::vector<RecognitionEvent> events_;
};

// ---------------------------------------------------------------------------
// Backend: owns recognizer (lazy init) and shared VAD.
// ---------------------------------------------------------------------------
class SherpaOfflineBackend : public AsrBackend {
public:
  SherpaOfflineBackend(ModelInfo model_info, AsrConfig asr_config, std::string provider_id)
      : model_info_(std::move(model_info)), asr_config_(std::move(asr_config)),
        provider_id_(std::move(provider_id)) {}

  SherpaOfflineBackend(const SherpaOfflineBackend&) = delete;
  SherpaOfflineBackend& operator=(const SherpaOfflineBackend&) = delete;
  SherpaOfflineBackend(SherpaOfflineBackend&&) = delete;
  SherpaOfflineBackend& operator=(SherpaOfflineBackend&&) = delete;

  ~SherpaOfflineBackend() override = default;

  [[nodiscard]] BackendDescriptor Describe() const override {
    BackendDescriptor descriptor;
    descriptor.provider_id = provider_id_;
    descriptor.provider_type = vinput::asr::kLocalProviderType;
    descriptor.backend_id = "sherpa-offline";
    descriptor.capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
    descriptor.capabilities.supports_hotwords = model_info_.supports_hotwords;
    return descriptor;
  }

  std::unique_ptr<RecognitionSession> CreateSession(std::string* error) override {
    const std::scoped_lock lock(mutex_);
    if (!InitializeLocked(error) || !recognizer_.has_value()) {
      return nullptr;
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<SherpaOfflineSession>(&*recognizer_, vad_.Available(), &vad_);
  }

private:
  bool InitializeLocked(std::string* error) {
    if (recognizer_.has_value() && recognizer_->Get() != nullptr) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    auto recognizer = CreateOfflineRecognizer(model_info_, asr_config_, error);
    if (!recognizer.has_value() || recognizer->Get() == nullptr) {
      return false;
    }
    recognizer_ = std::move(recognizer);

    if (asr_config_.vad_enabled && !asr_config_.vad_model_path.empty()) {
      const std::string vad_provider = JsonString(model_info_.model_config, "provider", "cpu");
      VadTrimParams vad_params;
      vad_params.threshold = asr_config_.vad_threshold;
      vad_params.min_speech_duration = asr_config_.vad_min_speech_duration;
      vad_params.min_silence_duration = asr_config_.vad_min_silence_duration;
      vad_params.speech_pad_ms = asr_config_.vad_speech_pad_ms;
      std::string vad_error;
      if (!vad_.Init(asr_config_.vad_model_path, 16000, vad_provider, vad_params, &vad_error)) {
        fprintf(stderr, "vinput: %s, continuing without VAD\n",
                vad_error.empty() ? "VAD model not available" : vad_error.c_str());
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  ModelInfo model_info_;
  AsrConfig asr_config_;
  std::string provider_id_;
  std::mutex mutex_;
  std::optional<sherpa_onnx::cxx::OfflineRecognizer> recognizer_;
  VadTrimmer vad_;
};

} // namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<AsrBackend> CreateSherpaOfflineBackend(const CoreConfig& config,
                                                       const LocalAsrProvider& provider,
                                                       LocalAsrModelContext context,
                                                       std::string* error) {
  context.asr_config.vad_enabled = config.asr.vad.enabled;
  context.asr_config.vad_model_path = VINPUT_VAD_MODEL_PATH;
  context.asr_config.vad_threshold = static_cast<float>(config.asr.vad.threshold);
  context.asr_config.vad_min_speech_duration = static_cast<float>(config.asr.vad.minSpeechDuration);
  context.asr_config.vad_min_silence_duration =
      static_cast<float>(config.asr.vad.minSilenceDuration);
  context.asr_config.vad_speech_pad_ms = config.asr.vad.speechPadMs;

  if (error != nullptr) {
    error->clear();
  }
  return std::make_unique<SherpaOfflineBackend>(context.info, std::move(context.asr_config),
                                                provider.id);
}

} // namespace vinput::daemon::asr
