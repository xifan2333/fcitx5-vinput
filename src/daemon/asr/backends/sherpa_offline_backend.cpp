#include "daemon/asr/backends/sherpa_offline_backend.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sherpa-onnx/c-api/c-api.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/asr/model_manager.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"
#include "common/utils/debug_log.h"
#include "common/utils/file_utils.h"

#include "daemon/asr/asr_config.h"
#include "daemon/asr/hotword_utils.h"
#include "daemon/asr/local_asr_model_loader.h"
#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/sherpa_capi.h"
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
// Context holding string buffers required for the lifetime of C API config
// pointers.
// ---------------------------------------------------------------------------
struct RecognizerConfigContext {
  std::string tokens_path;
  std::string f_model;
  std::string f_encoder;
  std::string f_decoder;
  std::string f_joiner;
  std::string f_preprocessor;
  std::string f_uncached_decoder;
  std::string f_cached_decoder;
  std::string f_merged_decoder;
  std::string f_encoder_adapter;
  std::string f_llm;
  std::string f_embedding;
  std::string f_tokenizer;
  std::string f_conv_frontend;
  std::string f_lm;
  std::string f_hotwords_file;
  std::string f_bpe_vocab;
  std::string f_rule_fsts;
  std::string f_rule_fars;

  std::string p_decoding_method;
  std::string p_provider;
  std::string explicit_model_type;
  std::string p_modeling_unit;

  std::string normalized_transducer_hotwords;
  std::string provider_prompt_hotwords;

  std::string cfg_language;
  std::string cfg_task;
  std::string cfg_src_lang;
  std::string cfg_tgt_lang;
  std::string cfg_system_prompt;
  std::string cfg_user_prompt;
  std::string cfg_hotwords;
  std::string cfg_telespeech_path;
  std::string temporary_hotwords_file;
};

void ResolveAssetPaths(const ModelInfo& info, RecognizerConfigContext& ctx) {
  ctx.tokens_path = info.File("tokens");
  ctx.f_model = info.File("model");
  ctx.f_encoder = info.File("encoder");
  ctx.f_decoder = info.File("decoder");
  ctx.f_joiner = info.File("joiner");
  ctx.f_preprocessor = info.File("preprocessor");
  ctx.f_uncached_decoder = info.File("uncached_decoder");
  ctx.f_cached_decoder = info.File("cached_decoder");
  ctx.f_merged_decoder = info.File("merged_decoder");
  ctx.f_encoder_adapter = info.File("encoder_adapter");
  ctx.f_llm = info.File("llm");
  ctx.f_embedding = info.File("embedding");
  ctx.f_tokenizer = info.File("tokenizer");
  ctx.f_conv_frontend = info.File("conv_frontend");
  ctx.f_lm = info.File("lm");
  ctx.f_hotwords_file = info.File("hotwords_file");
  ctx.f_bpe_vocab = info.File("bpe_vocab");
  ctx.f_rule_fsts = info.File("rule_fsts");
  ctx.f_rule_fars = info.File("rule_fars");
}

void ConfigureCommonSettings(const ModelInfo& info, const AsrConfig& asr_config,
                             RecognizerConfigContext& ctx,
                             SherpaOnnxOfflineRecognizerConfig& config) {
  const auto& recognizer_cfg = info.recognizer_config;
  const auto& model_cfg = info.model_config;
  const auto& feat_cfg = vinput::json::Object(recognizer_cfg, "feat_config");
  const auto& lm_cfg = vinput::json::Object(recognizer_cfg, "lm_config");

  config.feat_config.sample_rate = JsonInt(feat_cfg, "sample_rate", 16000);
  config.feat_config.feature_dim = JsonInt(feat_cfg, "feature_dim", 80);

  ctx.p_decoding_method = JsonString(recognizer_cfg, "decoding_method", "greedy_search");
  config.decoding_method = ctx.p_decoding_method.c_str();
  config.max_active_paths = JsonInt(recognizer_cfg, "max_active_paths", 4);
  config.hotwords_score = JsonFloat(recognizer_cfg, "hotwords_score", 1.5F);
  config.blank_penalty = JsonFloat(recognizer_cfg, "blank_penalty", 0.0F);

  if (!ctx.tokens_path.empty()) {
    config.model_config.tokens = ctx.tokens_path.c_str();
  }
  config.model_config.num_threads = JsonInt(model_cfg, "num_threads", asr_config.thread_num);
  ctx.p_provider = JsonString(model_cfg, "provider", "cpu");
  config.model_config.provider = ctx.p_provider.c_str();

  ctx.explicit_model_type = JsonString(model_cfg, "model_type");
  if (!ctx.explicit_model_type.empty()) {
    config.model_config.model_type = ctx.explicit_model_type.c_str();
  }

  if (!ctx.f_bpe_vocab.empty()) {
    config.model_config.bpe_vocab = ctx.f_bpe_vocab.c_str();
  }
  ctx.p_modeling_unit = JsonString(model_cfg, "modeling_unit", "cjkchar");
  if (!ctx.p_modeling_unit.empty()) {
    config.model_config.modeling_unit = ctx.p_modeling_unit.c_str();
  }

  if (!ctx.f_lm.empty()) {
    config.lm_config.model = ctx.f_lm.c_str();
    config.lm_config.scale = JsonFloat(lm_cfg, "scale", 0.5F);
  }

  if (!ctx.f_rule_fsts.empty()) {
    config.rule_fsts = ctx.f_rule_fsts.c_str();
  }
  if (!ctx.f_rule_fars.empty()) {
    config.rule_fars = ctx.f_rule_fars.c_str();
  }
}

bool ConfigureTransducerHotwords(const ModelInfo& info, std::string_view configured_hotwords,
                                 std::string_view fallback_hotwords,
                                 SherpaOnnxOfflineRecognizerConfig& config,
                                 std::string* normalized_hotwords, std::string* error) {
  if (!info.supports_hotwords || !IsTransducerHotwordFamily(info.family)) {
    return true;
  }
  const std::string_view selected =
      configured_hotwords.empty() ? fallback_hotwords : configured_hotwords;
  if (selected.empty()) {
    return true;
  }
  if (!LoadTransducerHotwords(std::string(selected), normalized_hotwords, error)) {
    return false;
  }
  if (normalized_hotwords->empty()) {
    return true;
  }
  if (!ValidateTransducerHotwordAssets(info, error)) {
    return false;
  }
  config.decoding_method = "modified_beam_search";
  return true;
}

bool ConfigureHotwordSettings(const ModelInfo& info, const AsrConfig& asr_config,
                              RecognizerConfigContext& ctx,
                              SherpaOnnxOfflineRecognizerConfig& config, std::string* error) {
  const bool provider_hotwords_configured =
      info.supports_hotwords && !asr_config.hotwords_file.empty();

  if (provider_hotwords_configured && IsPromptHotwordFamily(info.family)) {
    if (!LoadPromptHotwordsCsv(asr_config.hotwords_file, &ctx.provider_prompt_hotwords, error)) {
      return false;
    }
  } else if (provider_hotwords_configured && !IsTransducerHotwordFamily(info.family)) {
    if (error != nullptr) {
      *error =
          "model family '" + info.family + "' does not expose an ASR hotword input in sherpa-onnx";
    }
    return false;
  }

  return ConfigureTransducerHotwords(info, asr_config.hotwords_file, ctx.f_hotwords_file, config,
                                     &ctx.normalized_transducer_hotwords, error);
}

bool TryConfigureSingleModel(std::string_view family, const std::string& model_path,
                             SherpaOnnxOfflineRecognizerConfig& config) {
  const char* path = model_path.c_str();
  if (family == "paraformer") {
    config.model_config.paraformer.model = path;
    return true;
  }
  if (family == "zipformer_ctc") {
    config.model_config.zipformer_ctc.model = path;
    return true;
  }
  if (family == "fire_red_asr_ctc") {
    config.model_config.fire_red_asr_ctc.model = path;
    return true;
  }
  if (family == "dolphin") {
    config.model_config.dolphin.model = path;
    return true;
  }
  if (family == "nemo_ctc") {
    config.model_config.nemo_ctc.model = path;
    return true;
  }
  if (family == "wenet_ctc") {
    config.model_config.wenet_ctc.model = path;
    return true;
  }
  if (family == "tdnn") {
    config.model_config.tdnn.model = path;
    return true;
  }
  if (family == "omnilingual") {
    config.model_config.omnilingual.model = path;
    return true;
  }
  if (family == "medasr") {
    config.model_config.medasr.model = path;
    return true;
  }
  return false;
}

bool TryConfigureEncoderDecoder(std::string_view family, const std::string& encoder,
                                const std::string& decoder, const std::string& joiner,
                                SherpaOnnxOfflineRecognizerConfig& config) {
  if (family == "transducer" || family == "nemo_transducer") {
    config.model_config.transducer.encoder = encoder.c_str();
    config.model_config.transducer.decoder = decoder.c_str();
    config.model_config.transducer.joiner = joiner.c_str();
    return true;
  }
  if (family == "fire_red_asr") {
    config.model_config.fire_red_asr.encoder = encoder.c_str();
    config.model_config.fire_red_asr.decoder = decoder.c_str();
    return true;
  }
  return false;
}

void ConfigureSenseVoice(const glz::json_t& family_cfg, const std::string& fallback_lang,
                         RecognizerConfigContext& ctx, SherpaOnnxOfflineRecognizerConfig& config) {
  config.model_config.sense_voice.model = ctx.f_model.c_str();
  ctx.cfg_language = JsonString(family_cfg, "language", fallback_lang);
  config.model_config.sense_voice.language = ctx.cfg_language.c_str();
  config.model_config.sense_voice.use_itn = JsonBool(family_cfg, "use_itn") ? 1 : 0;
}

void ConfigureWhisper(const glz::json_t& family_cfg, const std::string& fallback_lang,
                      RecognizerConfigContext& ctx, SherpaOnnxOfflineRecognizerConfig& config) {
  config.model_config.whisper.encoder = ctx.f_encoder.c_str();
  config.model_config.whisper.decoder = ctx.f_decoder.c_str();
  ctx.cfg_language = JsonString(family_cfg, "language", fallback_lang);
  ctx.cfg_task = JsonString(family_cfg, "task", "transcribe");
  config.model_config.whisper.language = ctx.cfg_language.c_str();
  config.model_config.whisper.task = ctx.cfg_task.c_str();
  config.model_config.whisper.tail_paddings = JsonInt(family_cfg, "tail_paddings", -1);
  config.model_config.whisper.enable_token_timestamps =
      JsonBool(family_cfg, "enable_token_timestamps") ? 1 : 0;
  config.model_config.whisper.enable_segment_timestamps =
      JsonBool(family_cfg, "enable_segment_timestamps") ? 1 : 0;
}

void ConfigureMoonshine(RecognizerConfigContext& ctx, SherpaOnnxOfflineRecognizerConfig& config) {
  config.model_config.moonshine.preprocessor = ctx.f_preprocessor.c_str();
  config.model_config.moonshine.encoder = ctx.f_encoder.c_str();
  config.model_config.moonshine.uncached_decoder = ctx.f_uncached_decoder.c_str();
  config.model_config.moonshine.cached_decoder = ctx.f_cached_decoder.c_str();
  if (!ctx.f_merged_decoder.empty()) {
    config.model_config.moonshine.merged_decoder = ctx.f_merged_decoder.c_str();
  }
}

void ConfigureCanary(const glz::json_t& family_cfg, const std::string& fallback_lang,
                     const std::string& asr_lang, RecognizerConfigContext& ctx,
                     SherpaOnnxOfflineRecognizerConfig& config) {
  config.model_config.canary.encoder = ctx.f_encoder.c_str();
  config.model_config.canary.decoder = ctx.f_decoder.c_str();
  ctx.cfg_src_lang = JsonString(family_cfg, "src_lang", fallback_lang);
  ctx.cfg_tgt_lang = JsonString(family_cfg, "tgt_lang", asr_lang);
  config.model_config.canary.src_lang = ctx.cfg_src_lang.c_str();
  config.model_config.canary.tgt_lang = ctx.cfg_tgt_lang.c_str();
  config.model_config.canary.use_pnc = JsonBool(family_cfg, "use_pnc") ? 1 : 0;
}

void ConfigureFunasrNano(const glz::json_t& family_cfg, const AsrConfig& asr_config,
                         const ModelInfo& info, RecognizerConfigContext& ctx,
                         SherpaOnnxOfflineRecognizerConfig& config) {
  config.model_config.funasr_nano.encoder_adaptor = ctx.f_encoder_adapter.c_str();
  config.model_config.funasr_nano.llm = ctx.f_llm.c_str();
  config.model_config.funasr_nano.embedding = ctx.f_embedding.c_str();
  config.model_config.funasr_nano.tokenizer = ctx.f_tokenizer.c_str();
  ctx.cfg_language = JsonString(family_cfg, "language", asr_config.language);
  config.model_config.funasr_nano.language = ctx.cfg_language.c_str();
  config.model_config.funasr_nano.itn = JsonBool(family_cfg, "itn") ? 1 : 0;

  ctx.cfg_system_prompt = JsonString(family_cfg, "system_prompt");
  if (!ctx.cfg_system_prompt.empty()) {
    config.model_config.funasr_nano.system_prompt = ctx.cfg_system_prompt.c_str();
  }
  ctx.cfg_user_prompt = JsonString(family_cfg, "user_prompt");
  if (!ctx.cfg_user_prompt.empty()) {
    config.model_config.funasr_nano.user_prompt = ctx.cfg_user_prompt.c_str();
  }

  const bool provider_hotwords_configured =
      info.supports_hotwords && !asr_config.hotwords_file.empty();
  ctx.cfg_hotwords = provider_hotwords_configured ? ctx.provider_prompt_hotwords
                                                  : JsonString(family_cfg, "hotwords");
  if (!ctx.cfg_hotwords.empty()) {
    config.model_config.funasr_nano.hotwords = ctx.cfg_hotwords.c_str();
  }
  config.model_config.funasr_nano.max_new_tokens = JsonInt(family_cfg, "max_new_tokens", 1024);
  config.model_config.funasr_nano.temperature = JsonFloat(family_cfg, "temperature", 1.0F);
  config.model_config.funasr_nano.top_p = JsonFloat(family_cfg, "top_p", 0.9F);
  config.model_config.funasr_nano.seed = JsonInt(family_cfg, "seed", 0);
}

void ConfigureQwen3Asr(const glz::json_t& family_cfg, const AsrConfig& asr_config,
                       const ModelInfo& info, RecognizerConfigContext& ctx,
                       SherpaOnnxOfflineRecognizerConfig& config) {
  config.model_config.qwen3_asr.conv_frontend = ctx.f_conv_frontend.c_str();
  config.model_config.qwen3_asr.encoder = ctx.f_encoder.c_str();
  config.model_config.qwen3_asr.decoder = ctx.f_decoder.c_str();
  config.model_config.qwen3_asr.tokenizer = ctx.f_tokenizer.c_str();
  config.model_config.qwen3_asr.max_total_len = JsonInt(family_cfg, "max_total_len", 4096);
  config.model_config.qwen3_asr.max_new_tokens = JsonInt(family_cfg, "max_new_tokens", 1024);
  config.model_config.qwen3_asr.temperature = JsonFloat(family_cfg, "temperature", 1.0F);
  config.model_config.qwen3_asr.top_p = JsonFloat(family_cfg, "top_p", 0.9F);
  config.model_config.qwen3_asr.seed = JsonInt(family_cfg, "seed", 0);

  const bool provider_hotwords_configured =
      info.supports_hotwords && !asr_config.hotwords_file.empty();
  ctx.cfg_hotwords = provider_hotwords_configured ? ctx.provider_prompt_hotwords
                                                  : JsonString(family_cfg, "hotwords");
  if (!ctx.cfg_hotwords.empty()) {
    config.model_config.qwen3_asr.hotwords = ctx.cfg_hotwords.c_str();
  }
}

bool ConfigureSpecialModelFamily(const ModelInfo& info, const AsrConfig& asr_config,
                                 const glz::json_t& family_cfg, RecognizerConfigContext& ctx,
                                 SherpaOnnxOfflineRecognizerConfig& config) {
  const auto& family = info.family;
  if (family == "sense_voice") {
    ConfigureSenseVoice(family_cfg, asr_config.language, ctx, config);
    return true;
  }
  if (family == "whisper") {
    ConfigureWhisper(family_cfg, asr_config.language, ctx, config);
    return true;
  }
  if (family == "canary") {
    ConfigureCanary(family_cfg, asr_config.language, asr_config.language, ctx, config);
    return true;
  }
  if (family == "moonshine") {
    ConfigureMoonshine(ctx, config);
    return true;
  }
  if (family == "funasr_nano") {
    ConfigureFunasrNano(family_cfg, asr_config, info, ctx, config);
    return true;
  }
  if (family == "qwen3_asr") {
    ConfigureQwen3Asr(family_cfg, asr_config, info, ctx, config);
    return true;
  }
  if (family == "telespeech_ctc") {
    ctx.cfg_telespeech_path = info.File("telespeech_ctc");
    config.model_config.telespeech_ctc = ctx.cfg_telespeech_path.c_str();
    return true;
  }
  return false;
}

bool ConfigureModelFamily(const ModelInfo& info, const AsrConfig& asr_config,
                          RecognizerConfigContext& ctx, SherpaOnnxOfflineRecognizerConfig& config,
                          std::string* error) {
  const auto& family = info.family;
  const auto& family_cfg = vinput::json::Object(info.model_config, family);

  const bool configured =
      TryConfigureSingleModel(family, ctx.f_model, config) ||
      TryConfigureEncoderDecoder(family, ctx.f_encoder, ctx.f_decoder, ctx.f_joiner, config) ||
      ConfigureSpecialModelFamily(info, asr_config, family_cfg, ctx, config);

  if (!configured) {
    if (error != nullptr) {
      *error = "unsupported model family '" + family + "'";
    }
    return false;
  }

  if (config.model_config.model_type == nullptr) {
    config.model_config.model_type = family.c_str();
  }
  return true;
}

std::optional<sherpa::OfflineRecognizer>
CreateRecognizerInstance(const ModelInfo& info, const AsrConfig& asr_config,
                         RecognizerConfigContext& ctx, SherpaOnnxOfflineRecognizerConfig& config,
                         std::string* error) {
  if (!ctx.normalized_transducer_hotwords.empty()) {
    if (!WriteTemporaryHotwordFile(ctx.normalized_transducer_hotwords, &ctx.temporary_hotwords_file,
                                   error)) {
      return std::nullopt;
    }
    config.hotwords_file = ctx.temporary_hotwords_file.c_str();
  }

  auto recognizer = sherpa::OfflineRecognizer::Create(config);
  if (!ctx.temporary_hotwords_file.empty()) {
    std::error_code remove_error;
    std::filesystem::remove(ctx.temporary_hotwords_file, remove_error);
    if (remove_error) {
      VLOG_WARN("failed to remove temporary hotwords file: {}", remove_error.message());
    }
  }

  if (!recognizer.has_value() || !recognizer->Valid()) {
    if (error != nullptr) {
      *error = "failed to create sherpa-onnx recognizer for family '" + info.family + "'";
    }
    return std::nullopt;
  }

  VLOG_INFO("sherpa-onnx offline ASR initialized (family: {}, lang: {})", info.family,
            asr_config.language);
  return recognizer;
}

std::optional<sherpa::OfflineRecognizer>
CreateOfflineRecognizer(const ModelInfo& info, const AsrConfig& asr_config, std::string* error) {
  RecognizerConfigContext ctx;
  SherpaOnnxOfflineRecognizerConfig config = {};

  ResolveAssetPaths(info, ctx);
  ConfigureCommonSettings(info, asr_config, ctx, config);
  if (!ConfigureHotwordSettings(info, asr_config, ctx, config, error)) {
    return std::nullopt;
  }
  if (!ConfigureModelFamily(info, asr_config, ctx, config, error)) {
    return std::nullopt;
  }
  return CreateRecognizerInstance(info, asr_config, ctx, config, error);
}

// ---------------------------------------------------------------------------
// Run inference on a recognizer with pre-processed float samples.
// ---------------------------------------------------------------------------
std::string InferOffline(const sherpa::OfflineRecognizer* recognizer,
                         const std::vector<float>& samples) {
  if (recognizer == nullptr || !recognizer->Valid()) {
    return {};
  }

  auto stream = recognizer->CreateStream();
  if (!stream.Valid()) {
    return {};
  }

  stream.AcceptWaveform(16000, samples.data(), static_cast<int32_t>(samples.size()));
  recognizer->Decode(stream);

  return sherpa::OfflineRecognizer::GetText(stream);
}

void AppendSegmentText(std::string& text, std::string_view seg_text) {
  if (seg_text.empty()) {
    return;
  }
  auto is_ascii_alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  if (!text.empty() && is_ascii_alnum(text.back()) && is_ascii_alnum(seg_text.front())) {
    text += ' ';
  }
  text.append(seg_text);
}

std::string TranscribeVadSegments(const sherpa::OfflineRecognizer* recognizer,
                                  const std::vector<std::vector<float>>& segments) {
  std::string text;
  for (const auto& segment : segments) {
    if (!segment.empty()) {
      AppendSegmentText(text, InferOffline(recognizer, segment));
    }
  }
  return text;
}

std::optional<std::string> ExecuteAudioInference(const sherpa::OfflineRecognizer* recognizer,
                                                 bool vad_available, VadTrimmer* vad,
                                                 const std::vector<float>& samples,
                                                 std::string* error) {
  try {
    if (vad_available && vad != nullptr && vad->Available()) {
      const auto segments = vad->Segment(samples, 16000);
      if (segments.empty()) {
        VLOG_DEBUG("VAD found no speech segments, skipping ASR");
        return std::string{};
      }
      return TranscribeVadSegments(recognizer, segments);
    }
    return InferOffline(recognizer, samples);
  } catch (const std::exception& e) {
    const double duration_sec = static_cast<double>(samples.size()) / 16000.0;
    VLOG_ERROR("sherpa-onnx offline decode exception on audio ({:.1f}s): {}", duration_sec,
               e.what());
    const std::string friendly_err = FormatOfflineAsrError(e.what(), duration_sec);
    if (error != nullptr) {
      *error = friendly_err;
    }
    return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Session: buffers audio, runs inference on Finish().
// ---------------------------------------------------------------------------
class SherpaOfflineSession : public RecognitionSession {
public:
  SherpaOfflineSession(const sherpa::OfflineRecognizer* recognizer, bool vad_available,
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
      VLOG_DEBUG("skipping ASR for short audio: {} samples ({:.1f} ms)", samples_.size(),
                 static_cast<double>(samples_.size()) * 1000.0 / 16000.0);
      events_.push_back({RecognitionEventKind::Completed, {}, {}});
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    std::string inference_error;
    const auto text =
        ExecuteAudioInference(recognizer_, vad_available_, vad_, samples_, &inference_error);
    if (!text.has_value()) {
      if (error != nullptr) {
        *error = inference_error;
      }
      events_.push_back({RecognitionEventKind::Error, {}, inference_error});
      events_.push_back({RecognitionEventKind::Completed, {}, {}});
      return false;
    }

    if (!text->empty()) {
      events_.push_back({RecognitionEventKind::FinalText, std::move(*text), {}});
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
  const sherpa::OfflineRecognizer* recognizer_;
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
    if (recognizer_.has_value() && recognizer_->Valid()) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    auto recognizer = CreateOfflineRecognizer(model_info_, asr_config_, error);
    if (!recognizer.has_value() || !recognizer->Valid()) {
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
        VLOG_WARN("{}, continuing without VAD",
                  vad_error.empty() ? "VAD model not available" : vad_error);
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
  std::optional<sherpa::OfflineRecognizer> recognizer_;
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
