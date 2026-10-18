#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

#include "common/asr/model_manager.h"
#include "common/utils/file_utils.h"

#include "daemon/asr/hotword_utils.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

namespace {

void TestFamilyCheckers() {
  using namespace vinput::daemon::asr;

  // Transducer families
  TEST_ASSERT(IsTransducerHotwordFamily("transducer"));
  TEST_ASSERT(IsTransducerHotwordFamily("nemo_transducer"));
  TEST_ASSERT(!IsTransducerHotwordFamily("funasr_nano"));
  TEST_ASSERT(!IsTransducerHotwordFamily("qwen3_asr"));
  TEST_ASSERT(!IsTransducerHotwordFamily("whisper"));
  TEST_ASSERT(!IsTransducerHotwordFamily(""));

  // Prompt families
  TEST_ASSERT(IsPromptHotwordFamily("funasr_nano"));
  TEST_ASSERT(IsPromptHotwordFamily("qwen3_asr"));
  TEST_ASSERT(!IsPromptHotwordFamily("transducer"));
  TEST_ASSERT(!IsPromptHotwordFamily("nemo_transducer"));
  TEST_ASSERT(!IsPromptHotwordFamily("whisper"));
  TEST_ASSERT(!IsPromptHotwordFamily(""));
}

void TestEmptyAndNonexistentFiles() {
  using namespace vinput::daemon::asr;

  std::string hotwords;
  std::string error;

  // Empty path string
  TEST_ASSERT(LoadTransducerHotwords("", &hotwords, &error));
  TEST_ASSERT(hotwords.empty());
  TEST_ASSERT(error.empty());

  TEST_ASSERT(LoadPromptHotwordsCsv("", &hotwords, &error));
  TEST_ASSERT(hotwords.empty());
  TEST_ASSERT(error.empty());

  // Non-existent path
  TEST_ASSERT(!LoadTransducerHotwords("/path/to/nonexistent/hotwords.txt", &hotwords, &error));
  TEST_ASSERT(!error.empty());
  TEST_ASSERT(error.find("not a regular file") != std::string::npos);

  error.clear();
  TEST_ASSERT(!LoadPromptHotwordsCsv("/path/to/nonexistent/hotwords.txt", &hotwords, &error));
  TEST_ASSERT(!error.empty());
  TEST_ASSERT(error.find("not a regular file") != std::string::npos);

  // Directory path
  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);
  TEST_ASSERT(!LoadTransducerHotwords(temp_dir->Path().string(), &hotwords, &error));
  TEST_ASSERT(error.find("not a regular file") != std::string::npos);

  // Empty regular file
  const auto empty_file = temp_dir->Path() / "empty.txt";
  std::string write_err;
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(empty_file, "", &write_err));

  TEST_ASSERT(LoadTransducerHotwords(empty_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords.empty());
  TEST_ASSERT(error.empty());

  TEST_ASSERT(LoadPromptHotwordsCsv(empty_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords.empty());
  TEST_ASSERT(error.empty());
}

void TestValidHotwordsParsing() {
  using namespace vinput::daemon::asr;

  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);

  // Simple hotwords without weights
  const auto simple_file = temp_dir->Path() / "simple.txt";
  std::string write_err;
  TEST_ASSERT(
      vinput::file::AtomicWriteTextFile(simple_file, "apple\nbanana\ncherry\n", &write_err));

  std::string hotwords;
  std::string error;
  TEST_ASSERT(LoadTransducerHotwords(simple_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "apple\nbanana\ncherry\n");
  TEST_ASSERT(error.empty());

  TEST_ASSERT(LoadPromptHotwordsCsv(simple_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "apple,banana,cherry");
  TEST_ASSERT(error.empty());

  // Weighted hotwords with float scores (including hex floats)
  const auto weighted_file = temp_dir->Path() / "weighted.txt";
  const std::string content = "alpha:2.5\nbeta:+10.0\ngamma:-1.5\ndelta:0\nhex:0x1p+2\n";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(weighted_file, content, &write_err));

  TEST_ASSERT(LoadTransducerHotwords(weighted_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "alpha :2.5\nbeta :+10.0\ngamma :-1.5\ndelta :0\nhex :0x1p+2\n");
  TEST_ASSERT(error.empty());

  TEST_ASSERT(LoadPromptHotwordsCsv(weighted_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "alpha,beta,gamma,delta,hex");
  TEST_ASSERT(error.empty());

  // Mixed blank lines, whitespace trimming, and CRLF line endings
  const auto mixed_file = temp_dir->Path() / "mixed.txt";
  const std::string mixed_content = "  \r\n"
                                    "alpha:2.5\r\n"
                                    "\r\n"
                                    "  beta  \r\n"
                                    "\n"
                                    "gamma:3.0\n"
                                    "   \n";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(mixed_file, mixed_content, &write_err));

  TEST_ASSERT(LoadTransducerHotwords(mixed_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "alpha :2.5\nbeta\ngamma :3.0\n");

  TEST_ASSERT(LoadPromptHotwordsCsv(mixed_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "alpha,beta,gamma");

  // UTF-8 / CJK phrases
  const auto cjk_file = temp_dir->Path() / "cjk.txt";
  const std::string cjk_content = "语音输入:5.0\n开源项目:2.0\n自然语言\n";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(cjk_file, cjk_content, &write_err));

  TEST_ASSERT(LoadTransducerHotwords(cjk_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "语音输入 :5.0\n开源项目 :2.0\n自然语言\n");

  TEST_ASSERT(LoadPromptHotwordsCsv(cjk_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "语音输入,开源项目,自然语言");
}

void TestColonHandling() {
  using namespace vinput::daemon::asr;

  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);

  // Literal colons in terms that don't look like scores (e.g. URLs, C++ symbols)
  const auto literal_file = temp_dir->Path() / "literal.txt";
  const std::string literal_content = "https://example.com/api\nstd::vector\nprefix:suffix\n";
  std::string write_err;
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(literal_file, literal_content, &write_err));

  std::string hotwords;
  std::string error;
  TEST_ASSERT(LoadTransducerHotwords(literal_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "https://example.com/api\nstd::vector\nprefix:suffix\n");

  TEST_ASSERT(LoadPromptHotwordsCsv(literal_file.string(), &hotwords, &error));
  TEST_ASSERT(hotwords == "https://example.com/api,std::vector,prefix:suffix");

  // Invalid: space before colon ("word :2.5")
  const auto space_before_file = temp_dir->Path() / "space_before.txt";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(space_before_file, "word :2.5\n", &write_err));
  TEST_ASSERT(!LoadTransducerHotwords(space_before_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("invalid hotword score; use 'word:3.5' without spaces") !=
              std::string::npos);

  // Invalid: space after colon ("word: 2.5")
  const auto space_after_file = temp_dir->Path() / "space_after.txt";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(space_after_file, "word: 2.5\n", &write_err));
  TEST_ASSERT(!LoadTransducerHotwords(space_after_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("invalid hotword score; use 'word:3.5' without spaces") !=
              std::string::npos);

  // Invalid: trailing colon with no score ("word:")
  const auto trailing_colon_file = temp_dir->Path() / "trailing_colon.txt";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(trailing_colon_file, "word:\n", &write_err));
  TEST_ASSERT(!LoadTransducerHotwords(trailing_colon_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("invalid hotword score; use 'word:3.5' without spaces") !=
              std::string::npos);

  // Invalid: score with no word (":2.5")
  const auto no_word_file = temp_dir->Path() / "no_word.txt";
  TEST_ASSERT(vinput::file::AtomicWriteTextFile(no_word_file, ":2.5\n", &write_err));
  TEST_ASSERT(!LoadTransducerHotwords(no_word_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("hotword score has no preceding word or phrase") != std::string::npos);
}

void TestInvalidScores() {
  using namespace vinput::daemon::asr;

  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);

  auto check_invalid = [&](const std::string& line) {
    const auto test_file = temp_dir->Path() / "invalid_score.txt";
    std::string write_err;
    TEST_ASSERT(vinput::file::AtomicWriteTextFile(test_file, line + "\n", &write_err));
    std::string hotwords;
    std::string error;
    TEST_ASSERT(!LoadTransducerHotwords(test_file.string(), &hotwords, &error));
    TEST_ASSERT(error.find("invalid hotword score; use 'word:3.5' without spaces") !=
                std::string::npos);
  };

  // NaN variations
  check_invalid("word:nan");
  check_invalid("word:NaN");
  check_invalid("word:NAN");
  check_invalid("word:nan(123)");
  check_invalid("word:NaN(payload)");

  // Inf / Infinity variations
  check_invalid("word:inf");
  check_invalid("word:INF");
  check_invalid("word:Inf");
  check_invalid("word:infinity");
  check_invalid("word:Infinity");
  check_invalid("word:INFINITY");

  // Ill-formed numbers starting with digit/sign
  check_invalid("word:1.2.3");
  check_invalid("word:++1");
  check_invalid("word:+-1");
  check_invalid("word:-+1");
  check_invalid("word:--1");
  check_invalid("word:1e9999");
}

void TestUtf8Validation() {
  using namespace vinput::daemon::asr;

  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);

  std::string hotwords;
  std::string error;

  // Invalid UTF-8 sequence
  const auto bad_utf8_file = temp_dir->Path() / "bad_utf8.txt";
  {
    std::ofstream out(bad_utf8_file, std::ios::binary);
    const char bad_bytes[] = {'\xff', '\xfe'};
    out.write(bad_bytes, sizeof(bad_bytes));
  }
  TEST_ASSERT(!LoadTransducerHotwords(bad_utf8_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("invalid hotwords file at byte") != std::string::npos);

  // Embedded NUL
  const auto nul_file = temp_dir->Path() / "nul.txt";
  {
    std::ofstream out(nul_file, std::ios::binary);
    const char nul_bytes[] = {'h', 'e', 'l', 'l', 'o', '\0', 'w', 'o', 'r', 'l', 'd', '\n'};
    out.write(nul_bytes, sizeof(nul_bytes));
  }
  TEST_ASSERT(!LoadTransducerHotwords(nul_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("embedded NUL is not allowed") != std::string::npos);

  // Disallowed control character (e.g. 0x07 Bell)
  const auto ctrl_file = temp_dir->Path() / "ctrl.txt";
  {
    std::ofstream out(ctrl_file, std::ios::binary);
    out << "hello\x07world\n";
  }
  TEST_ASSERT(!LoadTransducerHotwords(ctrl_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("control character is not allowed") != std::string::npos);
}

void TestFileSizeLimit() {
  using namespace vinput::daemon::asr;

  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);

  // File size exceeding 1 MiB (1024 * 1024 + 16 bytes)
  const auto large_file = temp_dir->Path() / "too_large.txt";
  {
    std::ofstream out(large_file, std::ios::binary);
    const std::string chunk(static_cast<std::size_t>(64) * 1024, 'a');
    for (int i = 0; i < 17; ++i) {
      out << chunk;
    }
  }

  std::string hotwords;
  std::string error;
  TEST_ASSERT(!LoadTransducerHotwords(large_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("exceeds 1 MiB limit") != std::string::npos);

  // Boundary: File size exceeding 1 MiB by exactly 1 byte (1024 * 1024 + 1)
  const auto boundary_file = temp_dir->Path() / "boundary_large.txt";
  {
    std::ofstream out(boundary_file, std::ios::binary);
    const std::string chunk(static_cast<std::size_t>(64) * 1024, 'a');
    for (int i = 0; i < 16; ++i) {
      out << chunk;
    }
    out << 'b';
  }

  error.clear();
  TEST_ASSERT(!LoadTransducerHotwords(boundary_file.string(), &hotwords, &error));
  TEST_ASSERT(error.find("exceeds 1 MiB limit") != std::string::npos);
}

void TestValidateTransducerHotwordAssets() {
  using namespace vinput::daemon::asr;

  auto temp_dir = vinput::file::ScopedTempDir::Create();
  TEST_ASSERT(temp_dir != nullptr);

  ModelInfo info;
  std::string error;

  // Missing modeling_unit
  TEST_ASSERT(!ValidateTransducerHotwordAssets(info, &error));
  TEST_ASSERT(error.find("missing model.modeling_unit") != std::string::npos);

  // Unsupported modeling_unit
  info.model_config["modeling_unit"] = "invalid_unit";
  error.clear();
  TEST_ASSERT(!ValidateTransducerHotwordAssets(info, &error));
  TEST_ASSERT(error.find("unsupported sherpa-onnx hotword modeling_unit") != std::string::npos);

  // Supported cjkchar unit (no bpe_vocab required)
  info.model_config["modeling_unit"] = "cjkchar";
  error.clear();
  TEST_ASSERT(ValidateTransducerHotwordAssets(info, &error));
  TEST_ASSERT(error.empty());

  // BPE unit without bpe_vocab
  info.model_config["modeling_unit"] = "bpe";
  error.clear();
  TEST_ASSERT(!ValidateTransducerHotwordAssets(info, &error));
  TEST_ASSERT(error.find("requires model.bpe_vocab") != std::string::npos);

  // BPE unit with valid bpe_vocab file
  const auto vocab_file = temp_dir->Path() / "bpe.vocab";
  std::string write_err;
  TEST_ASSERT(
      vinput::file::AtomicWriteTextFile(vocab_file, "hello\t-1.0\nworld\t-2.0\n", &write_err));
  info.files["bpe_vocab"] = vocab_file.string();
  error.clear();
  TEST_ASSERT(ValidateTransducerHotwordAssets(info, &error));
  TEST_ASSERT(error.empty());
}

} // namespace

int main() {
  TestFamilyCheckers();
  TestEmptyAndNonexistentFiles();
  TestValidHotwordsParsing();
  TestColonHandling();
  TestInvalidScores();
  TestUtf8Validation();
  TestFileSizeLimit();
  TestValidateTransducerHotwordAssets();

  std::printf("All test_hotword_utils tests passed!\n");
  return 0;
}
