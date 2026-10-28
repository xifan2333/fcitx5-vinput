#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/asr/bpe_vocab_exporter.h"
#include "common/utils/expected.h"
#include "common/utils/file_utils.h"

namespace fs = std::filesystem;

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

std::vector<char> MakePieceBytes(std::string_view piece, float score) {
  std::vector<char> buf;
  // Field 1, wire 2 (piece)
  buf.push_back('\x0A');
  buf.push_back(static_cast<char>(piece.size()));
  for (const char c : piece) {
    buf.push_back(c);
  }
  // Field 2, wire 5 (score)
  buf.push_back('\x15');
  const auto bits = std::bit_cast<std::uint32_t>(score);
  buf.push_back(static_cast<char>(bits & 0xFF));
  buf.push_back(static_cast<char>((bits >> 8) & 0xFF));
  buf.push_back(static_cast<char>((bits >> 16) & 0xFF));
  buf.push_back(static_cast<char>((bits >> 24) & 0xFF));
  return buf;
}

std::vector<char> MakeModelProto(const std::vector<std::pair<std::string, float>>& pieces) {
  std::vector<char> buf;
  for (const auto& [piece, score] : pieces) {
    auto piece_bytes = MakePieceBytes(piece, score);
    // Field 1, wire 2 (SentencePiece message)
    buf.push_back('\x0A');
    buf.push_back(static_cast<char>(piece_bytes.size()));
    buf.insert(buf.end(), piece_bytes.begin(), piece_bytes.end());
  }
  return buf;
}

void TestParseSentencePieceValid() {
  const auto msg = MakePieceBytes("hello", -1.5F);
  auto res = vinput::asr::ParseSentencePiece(std::as_bytes(std::span(msg)));
  TEST_ASSERT(res.has_value());
  TEST_ASSERT(res->piece == "hello");
  TEST_ASSERT(res->score == -1.5F);

  // Piece only (no score field -> score defaults to 0.0)
  const std::vector<char> piece_only = {
      '\x0A', '\x03', 'f', 'o', 'o',
  };
  auto res2 = vinput::asr::ParseSentencePiece(std::as_bytes(std::span(piece_only)));
  TEST_ASSERT(res2.has_value());
  TEST_ASSERT(res2->piece == "foo");
  TEST_ASSERT(res2->score == 0.0F);

  // Piece with extra unknown fields (wire 0, wire 1, wire 5)
  std::vector<char> with_extra = piece_only;
  // Field 3, wire 0 (varint): key = (3<<3)|0 = 24 (0x18), value = 42
  with_extra.push_back('\x18');
  with_extra.push_back('\x2A');
  // Field 4, wire 5 (fixed32): key = (4<<3)|5 = 37 (0x25), value = 4 bytes
  with_extra.push_back('\x25');
  with_extra.push_back('\x01');
  with_extra.push_back('\x02');
  with_extra.push_back('\x03');
  with_extra.push_back('\x04');
  // Field 5, wire 1 (fixed64): key = (5<<3)|1 = 41 (0x29), value = 8 bytes
  with_extra.push_back('\x29');
  for (int i = 0; i < 8; ++i) {
    with_extra.push_back('\x01');
  }

  auto res3 = vinput::asr::ParseSentencePiece(std::as_bytes(std::span(with_extra)));
  TEST_ASSERT(res3.has_value());
  TEST_ASSERT(res3->piece == "foo");
}

void TestParseSentencePieceMalformed() {
  // Empty buffer -> missing piece
  const std::vector<char> empty;
  TEST_ASSERT(!vinput::asr::ParseSentencePiece(std::as_bytes(std::span(empty))).has_value());

  // Truncated varint (continuation bit set without following byte)
  const std::vector<char> trunc_varint = {'\x8A'};
  TEST_ASSERT(!vinput::asr::ParseSentencePiece(std::as_bytes(std::span(trunc_varint))).has_value());

  // 10th byte overflow in varint (low 7 bits > 1 at shift 63)
  const std::vector<char> overflow_varint = {
      '\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x02',
  };
  TEST_ASSERT(
      !vinput::asr::ParseSentencePiece(std::as_bytes(std::span(overflow_varint))).has_value());

  // Declared length exceeds buffer size
  const std::vector<char> trunc_len = {
      '\x0A',
      '\x0A',
      'a',
      'b',
  };
  TEST_ASSERT(!vinput::asr::ParseSentencePiece(std::as_bytes(std::span(trunc_len))).has_value());

  // Truncated wire 5 float (less than 4 bytes)
  const std::vector<char> trunc_float = {
      '\x15',
      '\x00',
      '\x00',
  };
  TEST_ASSERT(!vinput::asr::ParseSentencePiece(std::as_bytes(std::span(trunc_float))).has_value());

  // Missing piece string (score only)
  const std::vector<char> score_only = {
      '\x15', '\x00', '\x00', '\x00', '\x00',
  };
  TEST_ASSERT(!vinput::asr::ParseSentencePiece(std::as_bytes(std::span(score_only))).has_value());

  // Unsupported wire type 3 (deprecated start group)
  const std::vector<char> bad_wire = {'\x0B'};
  TEST_ASSERT(!vinput::asr::ParseSentencePiece(std::as_bytes(std::span(bad_wire))).has_value());
}

void TestValidateBpeVocabulary() {
  const auto tmp_dir = fs::temp_directory_path() / "vinput_test_bpe_validate";
  fs::create_directories(tmp_dir);

  // Non-existent file
  TEST_ASSERT(!ValidateBpeVocabulary(tmp_dir / "nonexistent.vocab").has_value());

  // Empty file
  const auto empty_path = tmp_dir / "empty.vocab";
  {
    const std::ofstream ofs(empty_path);
  }
  TEST_ASSERT(!ValidateBpeVocabulary(empty_path).has_value());

  // Valid vocab file
  const auto valid_path = tmp_dir / "valid.vocab";
  {
    std::ofstream ofs(valid_path);
    ofs << "<unk>\t0\n";
    ofs << "<s>\t0\r\n";
    ofs << "</s>\t0\n";
    ofs << " the\t-2.345\n";
    ofs << "apple\t-4.567\n";
  }
  auto valid_res = ValidateBpeVocabulary(valid_path);
  TEST_ASSERT(valid_res.has_value());

  // Missing tab
  const auto no_tab_path = tmp_dir / "no_tab.vocab";
  {
    std::ofstream ofs(no_tab_path);
    ofs << "apple -4.567\n";
  }
  TEST_ASSERT(!ValidateBpeVocabulary(no_tab_path).has_value());

  // Empty piece (tab at offset 0)
  const auto empty_piece_path = tmp_dir / "empty_piece.vocab";
  {
    std::ofstream ofs(empty_piece_path);
    ofs << "\t-4.567\n";
  }
  TEST_ASSERT(!ValidateBpeVocabulary(empty_piece_path).has_value());

  // Invalid non-float score
  const auto bad_score_path = tmp_dir / "bad_score.vocab";
  {
    std::ofstream ofs(bad_score_path);
    ofs << "apple\tnot_a_number\n";
  }
  TEST_ASSERT(!ValidateBpeVocabulary(bad_score_path).has_value());

  // Trailing garbage after score
  const auto garbage_score_path = tmp_dir / "garbage_score.vocab";
  {
    std::ofstream ofs(garbage_score_path);
    ofs << "apple\t-1.5extra\n";
  }
  TEST_ASSERT(!ValidateBpeVocabulary(garbage_score_path).has_value());

  fs::remove_all(tmp_dir);
}

void TestExportSentencePieceVocabularyEndToEnd() {
  const auto tmp_dir = fs::temp_directory_path() / "vinput_test_bpe_export";
  fs::create_directories(tmp_dir);

  const auto model_path = tmp_dir / "test.model";
  const auto vocab_path = tmp_dir / "test.vocab";

  // Build model with 3 pieces
  const std::vector<std::pair<std::string, float>> pieces = {
      {"<unk>", 0.0F},
      {" hello", -1.25F},
      {"world", -3.75F},
  };
  const auto model_data = MakeModelProto(pieces);
  {
    std::ofstream ofs(model_path, std::ios::binary);
    ofs.write(model_data.data(), static_cast<std::streamsize>(model_data.size()));
  }

  // Export
  auto export_res = ExportSentencePieceVocabulary(model_path, vocab_path);
  TEST_ASSERT(export_res.has_value());

  // Validate exported vocabulary
  auto valid_res = ValidateBpeVocabulary(vocab_path);
  TEST_ASSERT(valid_res.has_value());

  // Read and check content
  auto read_res = vinput::file::ReadTextFile(vocab_path);
  TEST_ASSERT(read_res.has_value());
  TEST_ASSERT(read_res->find("<unk>\t0\n") != std::string::npos);
  TEST_ASSERT(read_res->find(" hello\t-1.25\n") != std::string::npos);
  TEST_ASSERT(read_res->find("world\t-3.75\n") != std::string::npos);

  // A malformed later record must not replace an existing vocabulary.
  const auto partial_model_path = tmp_dir / "partial.model";
  const auto partial_vocab_path = tmp_dir / "partial.vocab";
  {
    std::vector<char> partial_model_data = MakeModelProto({{"valid", 0.0F}});
    partial_model_data.push_back('\x0A');
    partial_model_data.push_back('\x01');
    partial_model_data.push_back('\x8A'); // Truncated nested varint
    std::ofstream model_ofs(partial_model_path, std::ios::binary);
    model_ofs.write(partial_model_data.data(),
                    static_cast<std::streamsize>(partial_model_data.size()));
  }
  {
    std::ofstream vocab_ofs(partial_vocab_path);
    vocab_ofs << "sentinel\n";
  }
  TEST_ASSERT(!ExportSentencePieceVocabulary(partial_model_path, partial_vocab_path).has_value());
  auto sentinel_res = vinput::file::ReadTextFile(partial_vocab_path);
  TEST_ASSERT(sentinel_res.has_value());
  TEST_ASSERT(*sentinel_res == "sentinel\n");

  // Export model with piece containing illegal tab character
  const auto bad_model_path = tmp_dir / "bad.model";
  const auto bad_vocab_path = tmp_dir / "bad.vocab";
  const auto bad_model_data = MakeModelProto({{"bad\tpiece", 1.0F}});
  {
    std::ofstream ofs(bad_model_path, std::ios::binary);
    ofs.write(bad_model_data.data(), static_cast<std::streamsize>(bad_model_data.size()));
  }
  TEST_ASSERT(!ExportSentencePieceVocabulary(bad_model_path, bad_vocab_path).has_value());

  // Export non-existent model
  TEST_ASSERT(!ExportSentencePieceVocabulary(tmp_dir / "missing.model", vocab_path).has_value());

  fs::remove_all(tmp_dir);
}

} // namespace

int main() {
  TestParseSentencePieceValid();
  TestParseSentencePieceMalformed();
  TestValidateBpeVocabulary();
  TestExportSentencePieceVocabularyEndToEnd();
  return 0;
}
