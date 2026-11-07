#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "utils/formatter.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << "\n";
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestMeasureVisibleWidthAscii() {
  using vinput::cli::MeasureVisibleWidth;
  TEST_ASSERT(MeasureVisibleWidth("") == 0);
  TEST_ASSERT(MeasureVisibleWidth("hello") == 5);
  TEST_ASSERT(MeasureVisibleWidth("hello world!") == 12);
}

void TestMeasureVisibleWidthCjk() {
  using vinput::cli::MeasureVisibleWidth;
  // CJK characters are double-width (2 columns each)
  TEST_ASSERT(MeasureVisibleWidth("中文") == 4);
  TEST_ASSERT(MeasureVisibleWidth("测试123") == 7);
  TEST_ASSERT(MeasureVisibleWidth("管道过滤器") == 10);
  TEST_ASSERT(MeasureVisibleWidth("【标点】") == 8);
}

void TestMeasureVisibleWidthAnsiAndOsc8() {
  using vinput::cli::MeasureVisibleWidth;

  // SGR color escapes (e.g. bold, red, reset) must have 0 display width
  const std::string colored = "\033[31mRed\033[0m";
  TEST_ASSERT(MeasureVisibleWidth(colored) == 3);

  const std::string styled_cjk = "\033[1;32m启用\033[0m";
  TEST_ASSERT(MeasureVisibleWidth(styled_cjk) == 4);

  // OSC 8 terminal hyperlink: \033]8;;<URL>\a<LABEL>\033]8;;\a
  // The URL should not contribute to visible column width.
  const std::string long_url =
      "\033]8;;https://github.com/xifan2333/vinput-registry/blob/main/resources/adapters/cjk-trim/"
      "README.md\aOpen README\033]8;;\a";
  TEST_ASSERT(MeasureVisibleWidth(long_url) == 11);

  // OSC 8 with ST termination (\033\\ instead of \a)
  const std::string alt_terminator = "\033]8;;https://example.com\033\\Doc\033]8;;\033\\";
  TEST_ASSERT(MeasureVisibleWidth(alt_terminator) == 3);
}

void TestPadCellToVisibleWidth() {
  using vinput::cli::PadCellToVisibleWidth;
  TEST_ASSERT(PadCellToVisibleWidth("abc", 5) == "abc  ");
  TEST_ASSERT(PadCellToVisibleWidth("中文", 6) == "中文  ");
  TEST_ASSERT(PadCellToVisibleWidth("long_text", 4) == "long_text");
}

void TestRenderTable() {
  using vinput::cli::RenderTable;

  const std::vector<std::string> headers = {"ID", "类型", "说明", "文档"};
  const std::string link1 =
      "\033]8;;https://github.com/xifan2333/vinput-registry/blob/main/resources/adapters/cjk-trim/"
      "README.md\aOpen README\033]8;;\a";
  const std::string link2 =
      "\033]8;;https://github.com/xifan2333/vinput-registry/blob/main/resources/providers/"
      "sherpa-onnx/README.md\aOpen README\033]8;;\a";

  const std::vector<std::vector<std::string>> rows = {
      {"cjk-trim", "管道过滤器", "中文标点修剪与格式化", link1},
      {"sherpa-local", "本地ASR", "离线高精度语音识别引擎", link2},
  };

  const std::string rendered = RenderTable(headers, rows, /*use_color=*/false);
  TEST_ASSERT(!rendered.empty());

  // Verify that the long URL was not split by line breaks
  TEST_ASSERT(rendered.find("cjk-trim/README.md") != std::string::npos);
  TEST_ASSERT(rendered.find("sherpa-onnx/README.md") != std::string::npos);

  // Count lines: 1 header line + 2 data lines = 3 lines total
  size_t line_count = 0;
  for (char c : rendered) {
    if (c == '\n') {
      ++line_count;
    }
  }
  TEST_ASSERT(line_count == 3);
}

} // namespace

int main() {
  TestMeasureVisibleWidthAscii();
  TestMeasureVisibleWidthCjk();
  TestMeasureVisibleWidthAnsiAndOsc8();
  TestPadCellToVisibleWidth();
  TestRenderTable();
  std::cout << "All formatter tests passed successfully.\n";
  return 0;
}
