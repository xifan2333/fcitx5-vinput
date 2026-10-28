#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#include "common/utils/expected.h"

namespace vinput::asr {

struct SentencePieceRecord {
  std::string piece;
  float score{0.0F};
};

Expected<SentencePieceRecord> ParseSentencePiece(std::span<const std::byte> message);

} // namespace vinput::asr

// Exports the piece table embedded in a SentencePiece ModelProto to the text
// vocabulary format expected by sherpa-onnx's model.bpe_vocab option.
vinput::Expected<void> ExportSentencePieceVocabulary(const std::filesystem::path& model_path,
                                                     const std::filesystem::path& vocab_path);

// Checks the tab-separated piece/score format consumed by simple-sentencepiece.
vinput::Expected<void> ValidateBpeVocabulary(const std::filesystem::path& vocab_path);
