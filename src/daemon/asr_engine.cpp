#include "asr_engine.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

AsrEngine::AsrEngine() = default;

AsrEngine::~AsrEngine() { Shutdown(); }

bool AsrEngine::Init(const ModelInfo &info, const AsrConfig &asr_config) {
  if (initialized_) {
    return true;
  }

  SherpaOnnxOfflineRecognizerConfig config = {};
  config.feat_config.sample_rate = 16000;
  config.feat_config.feature_dim = 80;

  // Default decoding configs
  config.decoding_method = "greedy_search";
  config.max_active_paths = 4;
  config.blank_penalty = 0.0f;

  // Stash file paths to keep c_str() pointers alive through recognizer creation
  const std::string tokens_path = info.File("tokens");
  const std::string f_model = info.File("model");
  const std::string f_encoder = info.File("encoder");
  const std::string f_decoder = info.File("decoder");
  const std::string f_joiner = info.File("joiner");
  const std::string f_preprocessor = info.File("preprocessor");
  const std::string f_uncached_decoder = info.File("uncached_decoder");
  const std::string f_cached_decoder = info.File("cached_decoder");
  const std::string f_merged_decoder = info.File("merged_decoder");
  const std::string f_encoder_adaptor = info.File("encoder_adaptor");
  const std::string f_llm = info.File("llm");
  const std::string f_embedding = info.File("embedding");
  const std::string f_tokenizer = info.File("tokenizer");
  const std::string f_lm = info.File("lm");
  const std::string f_hotwords_file = info.File("hotwords_file");
  const std::string f_bpe_vocab = info.File("bpe_vocab");
  const std::string f_rule_fsts = info.File("rule_fsts");
  const std::string f_rule_fars = info.File("rule_fars");
  const std::string p_language = asr_config.language;
  const std::string p_modeling_unit = info.Param("modeling_unit", "cjkchar");
  const std::string p_tgt_lang = info.Param("tgt_lang", asr_config.language);
  const std::string p_system_prompt = info.Param("system_prompt");
  const std::string p_user_prompt = info.Param("user_prompt");
  const std::string p_hotwords = info.Param("hotwords");

  config.model_config.tokens = tokens_path.c_str();
  config.model_config.num_threads = asr_config.thread_num;
  config.model_config.provider = "cpu";


  // Optional general model config fields from params
  if (!f_bpe_vocab.empty()) {
    config.model_config.bpe_vocab = f_bpe_vocab.c_str();
  }

  // Optional LM config
  if (!f_lm.empty()) {
    config.lm_config.model = f_lm.c_str();
    config.lm_config.scale =
        std::stof(info.Param("lm_scale", "0.5"));
  }
  const auto &type = info.model_type;

  // Only paraformer and zipformer_transducer support hotwords_file
  // with modified_beam_search.
  const bool type_supports_hotwords = (type == "zipformer_transducer");
  if (type_supports_hotwords) {
    if (!asr_config.hotwords_file.empty()) {
      config.hotwords_file = asr_config.hotwords_file.c_str();
      config.decoding_method = "modified_beam_search";
    } else if (!f_hotwords_file.empty()) {
      config.hotwords_file = f_hotwords_file.c_str();
      config.decoding_method = "modified_beam_search";
    }
  }

  // Optional rule FSTs/FARs
  if (!f_rule_fsts.empty()) {
    config.rule_fsts = f_rule_fsts.c_str();
  }
  if (!f_rule_fars.empty()) {
    config.rule_fars = f_rule_fars.c_str();
  }

  if (type == "paraformer") {
    config.model_config.paraformer.model = f_model.c_str();
    config.model_config.model_type = "paraformer";
    config.model_config.modeling_unit = p_modeling_unit.c_str();

  } else if (type == "sense_voice") {
    config.model_config.sense_voice.model = f_model.c_str();
    config.model_config.sense_voice.language = p_language.c_str();
    config.model_config.sense_voice.use_itn =
        info.ParamBool("use_itn") ? 1 : 0;
    config.model_config.model_type = "sense_voice";

  } else if (type == "whisper") {
    config.model_config.whisper.encoder = f_encoder.c_str();
    config.model_config.whisper.decoder = f_decoder.c_str();
    config.model_config.whisper.language = p_language.c_str();
    config.model_config.whisper.task = "transcribe";
    config.model_config.whisper.tail_paddings =
        std::stoi(info.Param("tail_paddings", "-1"));
    config.model_config.whisper.enable_token_timestamps =
        info.ParamBool("enable_token_timestamps") ? 1 : 0;
    config.model_config.whisper.enable_segment_timestamps =
        info.ParamBool("enable_segment_timestamps") ? 1 : 0;
    config.model_config.model_type = "whisper";

  } else if (type == "moonshine") {
    config.model_config.moonshine.preprocessor = f_preprocessor.c_str();
    config.model_config.moonshine.encoder = f_encoder.c_str();
    config.model_config.moonshine.uncached_decoder =
        f_uncached_decoder.c_str();
    config.model_config.moonshine.cached_decoder = f_cached_decoder.c_str();
    if (!f_merged_decoder.empty()) {
      config.model_config.moonshine.merged_decoder = f_merged_decoder.c_str();
    }
    config.model_config.model_type = "moonshine";

  } else if (type == "zipformer_transducer") {
    config.model_config.transducer.encoder = f_encoder.c_str();
    config.model_config.transducer.decoder = f_decoder.c_str();
    config.model_config.transducer.joiner = f_joiner.c_str();
    config.model_config.model_type = "transducer";

  } else if (type == "zipformer_ctc") {
    config.model_config.zipformer_ctc.model = f_model.c_str();
    config.model_config.model_type = "zipformer_ctc";

  } else if (type == "fire_red_asr") {
    if (!f_encoder.empty()) {
      config.model_config.fire_red_asr.encoder = f_encoder.c_str();
      config.model_config.fire_red_asr.decoder = f_decoder.c_str();
      config.model_config.model_type = "fire_red_asr";
    } else {
      config.model_config.fire_red_asr_ctc.model = f_model.c_str();
      config.model_config.model_type = "fire_red_asr_ctc";
    }

  } else if (type == "dolphin") {
    config.model_config.dolphin.model = f_model.c_str();
    config.model_config.model_type = "dolphin";

  } else if (type == "nemo_ctc") {
    config.model_config.nemo_ctc.model = f_model.c_str();
    config.model_config.model_type = "nemo_ctc";

  } else if (type == "wenet_ctc") {
    config.model_config.wenet_ctc.model = f_model.c_str();
    config.model_config.model_type = "wenet_ctc";

  } else if (type == "tdnn") {
    config.model_config.tdnn.model = f_model.c_str();
    config.model_config.model_type = "tdnn";

  } else if (type == "telespeech_ctc") {
    config.model_config.telespeech_ctc = f_model.c_str();
    config.model_config.model_type = "telespeech_ctc";

  } else if (type == "omnilingual") {
    config.model_config.omnilingual.model = f_model.c_str();
    config.model_config.model_type = "omnilingual";

  } else if (type == "medasr") {
    config.model_config.medasr.model = f_model.c_str();
    config.model_config.model_type = "medasr";

  } else if (type == "canary") {
    config.model_config.canary.encoder = f_encoder.c_str();
    config.model_config.canary.decoder = f_decoder.c_str();
    config.model_config.canary.src_lang = p_language.c_str();
    config.model_config.canary.tgt_lang = p_tgt_lang.c_str();
    config.model_config.canary.use_pnc = info.ParamBool("use_pnc") ? 1 : 0;
    config.model_config.model_type = "canary";

  } else if (type == "funasr_nano") {
    config.model_config.funasr_nano.encoder_adaptor =
        f_encoder_adaptor.c_str();
    config.model_config.funasr_nano.llm = f_llm.c_str();
    config.model_config.funasr_nano.embedding = f_embedding.c_str();
    config.model_config.funasr_nano.tokenizer = f_tokenizer.c_str();
    config.model_config.funasr_nano.language = p_language.c_str();
    config.model_config.funasr_nano.itn =
        info.ParamBool("use_itn") ? 1 : 0;
    if (!p_system_prompt.empty()) {
      config.model_config.funasr_nano.system_prompt =
          p_system_prompt.c_str();
    }
    if (!p_user_prompt.empty()) {
      config.model_config.funasr_nano.user_prompt = p_user_prompt.c_str();
    }
    if (!p_hotwords.empty()) {
      config.model_config.funasr_nano.hotwords = p_hotwords.c_str();
    }
    config.model_config.funasr_nano.max_new_tokens =
        std::stoi(info.Param("max_new_tokens", "1024"));
    config.model_config.funasr_nano.temperature =
        std::stof(info.Param("temperature", "1.0"));
    config.model_config.funasr_nano.top_p =
        std::stof(info.Param("top_p", "0.9"));
    config.model_config.funasr_nano.seed =
        std::stoi(info.Param("seed", "0"));
    config.model_config.model_type = "funasr_nano";

  } else {
    fprintf(stderr, "vinput: unsupported model type '%s'\n", type.c_str());
    return false;
  }

  recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
  if (!recognizer_) {
    fprintf(stderr,
            "vinput: failed to create sherpa-onnx recognizer for type '%s'\n",
            type.c_str());
    return false;
  }

  initialized_ = true;
  fprintf(
      stderr,
      "vinput: sherpa-onnx ASR initialized successfully (type: %s, lang: %s)\n",
      type.c_str(), asr_config.language.c_str());
  return true;
}

std::string AsrEngine::Infer(const std::vector<int16_t> &pcm_data) {
  if (!initialized_ || pcm_data.empty()) {
    return "";
  }

  if (pcm_data.size() < kMinSamplesForInference) {
    fprintf(stderr,
            "vinput: skipping ASR for short audio: %zu samples (%.1f ms)\n",
            pcm_data.size(),
            static_cast<double>(pcm_data.size()) * 1000.0 / 16000.0);
    return "";
  }

  // sherpa-onnx expects float samples in [-1, 1]
  std::vector<float> samples(pcm_data.size());
  for (size_t i = 0; i < pcm_data.size(); ++i) {
    samples[i] = static_cast<float>(pcm_data[i]) / 32768.0f;
  }

  const SherpaOnnxOfflineStream *stream =
      SherpaOnnxCreateOfflineStream(recognizer_);
  if (!stream) {
    fprintf(stderr, "vinput: failed to create sherpa-onnx stream\n");
    return "";
  }

  SherpaOnnxAcceptWaveformOffline(stream, 16000, samples.data(),
                                  static_cast<int32_t>(samples.size()));
  SherpaOnnxDecodeOfflineStream(recognizer_, stream);

  const SherpaOnnxOfflineRecognizerResult *result =
      SherpaOnnxGetOfflineStreamResult(stream);
  std::string text;
  if (result && result->text) {
    text = result->text;
  }

  if (result) {
    SherpaOnnxDestroyOfflineRecognizerResult(result);
  }
  SherpaOnnxDestroyOfflineStream(stream);

  return text;
}

void AsrEngine::Shutdown() {
  if (initialized_) {
    SherpaOnnxDestroyOfflineRecognizer(recognizer_);
    recognizer_ = nullptr;
    initialized_ = false;
  }
  if (!hotwords_tmp_path_.empty()) {
    std::filesystem::remove(hotwords_tmp_path_);
    hotwords_tmp_path_.clear();
  }
}

bool AsrEngine::IsInitialized() const { return initialized_; }
