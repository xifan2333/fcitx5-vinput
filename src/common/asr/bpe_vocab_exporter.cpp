#include "common/asr/bpe_vocab_exporter.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "common/utils/expected.h"
#include "common/utils/file_utils.h"

namespace {

constexpr std::uintmax_t kMaxSentencePieceModelBytes = 64ULL * 1024 * 1024;
constexpr std::uintmax_t kMaxBpeVocabularyBytes = 256ULL * 1024 * 1024;
constexpr std::size_t kMaxPieceCount = 1000000;
constexpr std::size_t kMaxPieceBytes = 1024ULL * 1024;
constexpr std::size_t kInitialVocabBufferSize = 64ULL * 1024;

class ByteReader {
public:
  constexpr explicit ByteReader(std::span<const std::byte> buffer) noexcept : buffer_(buffer) {}

  [[nodiscard]] constexpr bool Empty() const noexcept { return buffer_.empty(); }
  [[nodiscard]] constexpr std::size_t Remaining() const noexcept { return buffer_.size(); }
  [[nodiscard]] constexpr std::span<const std::byte> RemainingSpan() const noexcept {
    return buffer_;
  }

  std::optional<std::uint64_t> ReadVarint() noexcept {
    std::uint64_t result = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (buffer_.empty()) {
        return std::nullopt;
      }
      const auto byte = static_cast<std::uint8_t>(buffer_.front());
      buffer_ = buffer_.subspan(1);
      result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) {
        return result;
      }
    }
    return std::nullopt;
  }

  std::optional<std::span<const std::byte>> ReadLengthDelimited() noexcept {
    const auto len = ReadVarint();
    if (!len.has_value() || *len > buffer_.size()) {
      return std::nullopt;
    }
    const auto count = static_cast<std::size_t>(*len);
    const auto sub = buffer_.first(count);
    buffer_ = buffer_.subspan(count);
    return sub;
  }

  std::optional<std::uint32_t> ReadFixed32() noexcept {
    if (buffer_.size() < 4) {
      return std::nullopt;
    }
    const auto b0 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer_[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer_[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer_[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer_[3]));
    buffer_ = buffer_.subspan(4);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
  }

  std::optional<float> ReadFloat() noexcept {
    const auto bits = ReadFixed32();
    if (!bits.has_value()) {
      return std::nullopt;
    }
    return std::bit_cast<float>(*bits);
  }

  bool Skip(std::size_t count) noexcept {
    if (buffer_.size() < count) {
      return false;
    }
    buffer_ = buffer_.subspan(count);
    return true;
  }

  bool SkipField(std::uint32_t wire_type) noexcept {
    switch (wire_type) {
    case 0:
      return ReadVarint().has_value();
    case 1:
      return Skip(8);
    case 2:
      return ReadLengthDelimited().has_value();
    case 5:
      return Skip(4);
    default:
      return false;
    }
  }

private:
  std::span<const std::byte> buffer_;
};

} // namespace

namespace vinput::asr {

Expected<SentencePieceRecord> ParseSentencePiece(std::span<const std::byte> message) {
  ByteReader reader(message);
  SentencePieceRecord record;
  bool found_piece = false;
  while (!reader.Empty()) {
    const auto key = reader.ReadVarint();
    if (!key.has_value()) {
      return UnexpectedError("invalid field key in piece record");
    }
    const auto field = static_cast<std::uint32_t>(*key >> 3);
    const auto wire = static_cast<std::uint32_t>(*key & 7);
    if (field == 1 && wire == 2) {
      const auto value = reader.ReadLengthDelimited();
      if (!value.has_value()) {
        return UnexpectedError("invalid length-delimited string in piece record");
      }
      record.piece.clear();
      record.piece.reserve(value->size());
      for (const auto b : *value) {
        record.piece.push_back(static_cast<char>(b));
      }
      found_piece = true;
    } else if (field == 2 && wire == 5) {
      const auto s = reader.ReadFloat();
      if (!s.has_value()) {
        return UnexpectedError("invalid float score in piece record");
      }
      record.score = *s;
    } else if (!reader.SkipField(wire)) {
      return UnexpectedError("unsupported wire type in piece record");
    }
  }
  if (!found_piece) {
    return UnexpectedError("missing piece text in piece record");
  }
  return record;
}

} // namespace vinput::asr

vinput::Expected<void> ExportSentencePieceVocabulary(const std::filesystem::path& model_path,
                                                     const std::filesystem::path& vocab_path) {
  std::error_code size_error;
  const auto model_size = std::filesystem::file_size(model_path, size_error);
  if (size_error) {
    return vinput::UnexpectedError("failed to inspect SentencePiece model: " +
                                   size_error.message());
  }
  if (model_size > kMaxSentencePieceModelBytes) {
    return vinput::UnexpectedError("SentencePiece model exceeds 64 MiB limit: " +
                                   model_path.string());
  }

  auto mapping_res = vinput::file::MapFileReadOnly(model_path);
  if (!mapping_res) {
    return vinput::UnexpectedError("failed to open SentencePiece model: " + mapping_res.error());
  }
  const auto& mapping = *mapping_res;
  const std::string_view data = mapping.View();

  std::string output_buffer;
  output_buffer.reserve(kInitialVocabBufferSize);

  ByteReader reader(std::as_bytes(std::span(data.data(), data.size())));
  std::size_t piece_count = 0;
  while (!reader.Empty()) {
    const auto key = reader.ReadVarint();
    if (!key.has_value()) {
      return vinput::UnexpectedError("invalid SentencePiece ModelProto: " + model_path.string());
    }
    const auto field = static_cast<std::uint32_t>(*key >> 3);
    const auto wire = static_cast<std::uint32_t>(*key & 7);
    if (field == 1 && wire == 2) {
      const auto message = reader.ReadLengthDelimited();
      if (!message.has_value()) {
        return vinput::UnexpectedError("invalid SentencePiece piece record: " +
                                       model_path.string());
      }
      auto record = vinput::asr::ParseSentencePiece(*message);
      if (!record) {
        return vinput::UnexpectedError("invalid SentencePiece piece record (" + record.error() +
                                       ") in: " + model_path.string());
      }
      if (record->piece.empty() || record->piece.size() > kMaxPieceBytes ||
          !std::isfinite(record->score) ||
          record->piece.find_first_of("\t\r\n") != std::string::npos) {
        return vinput::UnexpectedError("unsupported SentencePiece piece record in: " +
                                       model_path.string());
      }
      std::format_to(std::back_inserter(output_buffer), "{}\t{}\n", record->piece, record->score);
      ++piece_count;
      if (piece_count > kMaxPieceCount || output_buffer.size() > kMaxBpeVocabularyBytes) {
        return vinput::UnexpectedError("SentencePiece vocabulary exceeds safety limits: " +
                                       model_path.string());
      }
    } else if (!reader.SkipField(wire)) {
      return vinput::UnexpectedError("unsupported SentencePiece ModelProto field in: " +
                                     model_path.string());
    }
  }

  if (piece_count == 0) {
    return vinput::UnexpectedError("SentencePiece model contains no vocabulary pieces: " +
                                   model_path.string());
  }

  if (auto res = vinput::file::AtomicWriteTextFile(vocab_path, output_buffer); !res) {
    return vinput::UnexpectedError("failed to write BPE vocabulary: " + res.error());
  }
  return {};
}

vinput::Expected<void> ValidateBpeVocabulary(const std::filesystem::path& vocab_path) {
  std::error_code size_error;
  const auto vocab_size = std::filesystem::file_size(vocab_path, size_error);
  if (size_error) {
    return vinput::UnexpectedError("failed to inspect BPE vocabulary: " + size_error.message());
  }
  if (vocab_size == 0 || vocab_size > kMaxBpeVocabularyBytes) {
    return vinput::UnexpectedError("BPE vocabulary is empty or exceeds 256 MiB: " +
                                   vocab_path.string());
  }

  std::ifstream input(vocab_path, std::ios::binary);
  if (!input) {
    return vinput::UnexpectedError("failed to open BPE vocabulary: " + vocab_path.string());
  }

  std::string line;
  std::size_t piece_count = 0;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size() || tab > kMaxPieceBytes) {
      return vinput::UnexpectedError("invalid BPE vocabulary line in: " + vocab_path.string());
    }
    const std::string score_text = line.substr(tab + 1);
    float score = 0.0F;
    const auto [ptr, ec] =
        std::from_chars(score_text.data(), score_text.data() + score_text.size(), score);
    if (ec != std::errc{} || ptr != score_text.data() + score_text.size() ||
        !std::isfinite(score)) {
      return vinput::UnexpectedError("invalid BPE vocabulary score in: " + vocab_path.string());
    }
    if (++piece_count > kMaxPieceCount) {
      return vinput::UnexpectedError("BPE vocabulary has too many pieces: " + vocab_path.string());
    }
  }
  if ((!input.good() && !input.eof()) || piece_count == 0) {
    return vinput::UnexpectedError("failed to read BPE vocabulary: " + vocab_path.string());
  }
  return {};
}
