#include "common/asr/bpe_vocab_exporter.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <vector>

#include "common/utils/file_utils.h"

namespace {

constexpr std::uintmax_t kMaxSentencePieceModelBytes = 64 * 1024 * 1024;
constexpr std::uintmax_t kMaxBpeVocabularyBytes = 256 * 1024 * 1024;
constexpr std::size_t kMaxPieceCount = 1000000;
constexpr std::size_t kMaxPieceBytes = 1024 * 1024;

bool ReadVarint(std::string_view data, std::size_t* offset, std::uint64_t* value) {
  std::uint64_t result = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (*offset >= data.size()) {
      return false;
    }
    const std::uint8_t byte = static_cast<std::uint8_t>(data[(*offset)++]);
    result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

bool ReadLengthDelimited(std::string_view data, std::size_t* offset, std::string_view* value) {
  std::uint64_t length = 0;
  if (!ReadVarint(data, offset, &length) || length > data.size() - *offset) {
    return false;
  }
  *value = data.substr(*offset, static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

bool SkipField(std::string_view data, std::size_t* offset, std::uint32_t wire_type) {
  switch (wire_type) {
  case 0: {
    std::uint64_t ignored = 0;
    return ReadVarint(data, offset, &ignored);
  }
  case 1:
    if (data.size() - *offset < 8) {
      return false;
    }
    *offset += 8;
    return true;
  case 2: {
    std::string_view ignored;
    return ReadLengthDelimited(data, offset, &ignored);
  }
  case 5:
    if (data.size() - *offset < 4) {
      return false;
    }
    *offset += 4;
    return true;
  default:
    return false;
  }
}

bool ParseSentencePiece(std::string_view message, std::string* piece, float* score) {
  std::size_t offset = 0;
  bool found_piece = false;
  while (offset < message.size()) {
    std::uint64_t key = 0;
    if (!ReadVarint(message, &offset, &key)) {
      return false;
    }
    const std::uint32_t field = static_cast<std::uint32_t>(key >> 3);
    const std::uint32_t wire = static_cast<std::uint32_t>(key & 7);
    if (field == 1 && wire == 2) {
      std::string_view value;
      if (!ReadLengthDelimited(message, &offset, &value)) {
        return false;
      }
      piece->assign(value);
      found_piece = true;
    } else if (field == 2 && wire == 5) {
      if (message.size() - offset < 4) {
        return false;
      }
      const std::uint32_t bits =
          static_cast<std::uint8_t>(message[offset]) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[offset + 1])) << 8) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[offset + 2])) << 16) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[offset + 3])) << 24);
      *score = std::bit_cast<float>(bits);
      offset += 4;
    } else if (!SkipField(message, &offset, wire)) {
      return false;
    }
  }
  return found_piece;
}

} // namespace

bool ExportSentencePieceVocabulary(const std::filesystem::path& model_path,
                                   const std::filesystem::path& vocab_path, std::string* error) {
  std::error_code size_error;
  const auto model_size = std::filesystem::file_size(model_path, size_error);
  if (size_error || model_size > kMaxSentencePieceModelBytes) {
    if (error) {
      *error = size_error ? "failed to inspect SentencePiece model: " + size_error.message()
                          : "SentencePiece model exceeds 64 MiB limit: " + model_path.string();
    }
    return false;
  }

  std::ifstream input(model_path, std::ios::binary);
  if (!input) {
    if (error) {
      *error = "failed to open SentencePiece model: " + model_path.string();
    }
    return false;
  }
  std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    if (error) {
      *error = "failed to read SentencePiece model: " + model_path.string();
    }
    return false;
  }

  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<float>::max_digits10);

  std::size_t offset = 0;
  std::size_t piece_count = 0;
  while (offset < data.size()) {
    std::uint64_t key = 0;
    if (!ReadVarint(data, &offset, &key)) {
      if (error) {
        *error = "invalid SentencePiece ModelProto: " + model_path.string();
      }
      return false;
    }
    const std::uint32_t field = static_cast<std::uint32_t>(key >> 3);
    const std::uint32_t wire = static_cast<std::uint32_t>(key & 7);
    if (field == 1 && wire == 2) {
      std::string_view message;
      if (!ReadLengthDelimited(data, &offset, &message)) {
        if (error) {
          *error = "invalid SentencePiece piece record: " + model_path.string();
        }
        return false;
      }
      std::string piece;
      float score = 0;
      if (!ParseSentencePiece(message, &piece, &score) || piece.empty() ||
          piece.size() > kMaxPieceBytes || !std::isfinite(score) ||
          piece.find_first_of("\t\r\n") != std::string::npos) {
        if (error) {
          *error = "unsupported SentencePiece piece record in: " + model_path.string();
        }
        return false;
      }
      output << piece << '\t' << score << '\n';
      ++piece_count;
      if (piece_count > kMaxPieceCount ||
          output.tellp() > static_cast<std::streamoff>(kMaxBpeVocabularyBytes)) {
        if (error) {
          *error = "SentencePiece vocabulary exceeds safety limits: " + model_path.string();
        }
        return false;
      }
    } else if (!SkipField(data, &offset, wire)) {
      if (error) {
        *error = "unsupported SentencePiece ModelProto field in: " + model_path.string();
      }
      return false;
    }
  }

  if (piece_count == 0) {
    if (error) {
      *error = "SentencePiece model contains no vocabulary pieces: " + model_path.string();
    }
    return false;
  }

  if (auto res = vinput::file::AtomicWriteTextFile(vocab_path, output.str()); !res) {
    if (error) {
      *error = "failed to write BPE vocabulary: " + res.error();
    }
    return false;
  }
  if (error) {
    error->clear();
  }
  return true;
}

bool ValidateBpeVocabulary(const std::filesystem::path& vocab_path, std::string* error) {
  std::error_code size_error;
  const auto vocab_size = std::filesystem::file_size(vocab_path, size_error);
  if (size_error || vocab_size == 0 || vocab_size > kMaxBpeVocabularyBytes) {
    if (error) {
      *error = size_error ? "failed to inspect BPE vocabulary: " + size_error.message()
                          : "BPE vocabulary is empty or exceeds 256 MiB: " + vocab_path.string();
    }
    return false;
  }

  std::ifstream input(vocab_path, std::ios::binary);
  if (!input) {
    if (error) {
      *error = "failed to open BPE vocabulary: " + vocab_path.string();
    }
    return false;
  }

  std::string line;
  std::size_t piece_count = 0;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size() || tab > kMaxPieceBytes) {
      if (error) {
        *error = "invalid BPE vocabulary line in: " + vocab_path.string();
      }
      return false;
    }
    const std::string score_text = line.substr(tab + 1);
    char* end = nullptr;
    const float score = std::strtof(score_text.c_str(), &end);
    if (end != score_text.c_str() + score_text.size() || !std::isfinite(score)) {
      if (error) {
        *error = "invalid BPE vocabulary score in: " + vocab_path.string();
      }
      return false;
    }
    if (++piece_count > kMaxPieceCount) {
      if (error) {
        *error = "BPE vocabulary has too many pieces: " + vocab_path.string();
      }
      return false;
    }
  }
  if ((!input.good() && !input.eof()) || piece_count == 0) {
    if (error) {
      *error = "failed to read BPE vocabulary: " + vocab_path.string();
    }
    return false;
  }
  if (error) {
    error->clear();
  }
  return true;
}
