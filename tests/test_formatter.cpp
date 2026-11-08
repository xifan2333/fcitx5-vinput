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
  for (const char c : rendered) {
    if (c == '\n') {
      ++line_count;
    }
  }
  TEST_ASSERT(line_count == 3);
}

void TestRenderTableWithHyperlinkInMiddleColumn() {
  using vinput::cli::MeasureVisibleWidth;
  using vinput::cli::RenderTable;

  const std::vector<std::string> headers = {"NAME", "LINK", "STATUS"};
  const std::string link =
      "\033]8;;https://github.com/xifan2333/fcitx5-vinput/pull/574\aPull Request\033]8;;\a";

  const std::vector<std::vector<std::string>> rows = {
      {"item-1", link, "运行中"},
      {"long-item-2", "Plain Doc", "已停止"},
  };

  const std::string rendered = RenderTable(headers, rows, /*use_color=*/false);
  TEST_ASSERT(!rendered.empty());

  // Verify that both rows align properly after the middle column
  const size_t first_newline = rendered.find('\n');
  TEST_ASSERT(first_newline != std::string::npos);
  const size_t second_newline = rendered.find('\n', first_newline + 1);
  TEST_ASSERT(second_newline != std::string::npos);

  const std::string row1 = rendered.substr(first_newline + 1, second_newline - first_newline - 1);
  std::string row2 = rendered.substr(second_newline + 1);
  if (!row2.empty() && row2.back() == '\n') {
    row2.pop_back();
  }

  // The substring preceding "运行中" and "已停止" should have identical visible width
  const size_t idx1 = row1.rfind("运行中");
  const size_t idx2 = row2.rfind("已停止");
  TEST_ASSERT(idx1 != std::string::npos);
  TEST_ASSERT(idx2 != std::string::npos);

  const std::string prefix1 = row1.substr(0, idx1);
  const std::string prefix2 = row2.substr(0, idx2);
  TEST_ASSERT(MeasureVisibleWidth(prefix1) == MeasureVisibleWidth(prefix2));
}

void TestRenderTableWithNewlines() {
  using vinput::cli::RenderTable;

  const std::vector<std::string> headers = {"ID", "DESCRIPTION"};
  const std::vector<std::vector<std::string>> rows = {
      {"item", "Line 1\nLine 2\r\nLine 3\tTab"},
  };

  const std::string rendered = RenderTable(headers, rows, /*use_color=*/false);
  TEST_ASSERT(!rendered.empty());

  // Newlines within cell must be flattened into single line to avoid breaking the table row
  size_t line_count = 0;
  for (const char c : rendered) {
    if (c == '\n') {
      ++line_count;
    }
  }
  TEST_ASSERT(line_count == 2); // 1 header + 1 row
  TEST_ASSERT(rendered.find("Line 1 Line 2 Line 3 Tab") != std::string::npos);
}

void TestRenderTableWithColor() {
  using vinput::cli::RenderTable;

  const std::vector<std::string> headers = {"ID", "STATUS"};
  const std::vector<std::vector<std::string>> rows = {
      {"item", "active"},
  };

  const std::string colored = RenderTable(headers, rows, /*use_color=*/true);
  TEST_ASSERT(!colored.empty());

  // Entire header line must be wrapped with bold sequence
  const size_t first_newline = colored.find('\n');
  TEST_ASSERT(first_newline != std::string::npos);
  const std::string header_line = colored.substr(0, first_newline);
  const std::string data_line = colored.substr(first_newline + 1);

  TEST_ASSERT(header_line.starts_with("\033[1m"));
  TEST_ASSERT(header_line.ends_with("\033[0m"));

  // Data row must not contain header bold style
  TEST_ASSERT(data_line.find("\033[1m") == std::string::npos);
  TEST_ASSERT(data_line.find("item  active") != std::string::npos);
}

void TestRenderTableWithUnclosedEscapes() {
  using vinput::cli::RenderTable;

  // Row 1 contains unclosed hyperlink and unclosed color
  // Row 2 contains closed hyperlink with trailing text (must not be falsely identified as unclosed)
  const std::vector<std::string> headers = {"LINK", "COLOR", "NOTE"};
  const std::vector<std::vector<std::string>> rows = {
      {"\033]8;;https://example.com/unclosed\aUnclosedLink", "\033[31mUnclosedRed", "Plain"},
      {"\033]8;;https://example.com/closed\aDoc\033]8;;\a (v1.0)", "\033[32mGreen\033[0m ok",
       "Normal"},
  };

  const std::string rendered = RenderTable(headers, rows, /*use_color=*/false);
  TEST_ASSERT(!rendered.empty());

  // Unclosed link must be closed with terminator in output
  TEST_ASSERT(rendered.find("UnclosedLink\033]8;;\a") != std::string::npos);
  // Unclosed color must be reset with terminator in output
  TEST_ASSERT(rendered.find("UnclosedRed\033[0m") != std::string::npos);
  // Trailing text in closed link must be preserved
  TEST_ASSERT(rendered.find("Doc\033]8;;\a (v1.0)") != std::string::npos);
}

} // namespace

int main() {
  TestMeasureVisibleWidthAscii();
  TestMeasureVisibleWidthCjk();
  TestMeasureVisibleWidthAnsiAndOsc8();
  TestPadCellToVisibleWidth();
  TestRenderTable();
  TestRenderTableWithHyperlinkInMiddleColumn();
  TestRenderTableWithNewlines();
  TestRenderTableWithColor();
  TestRenderTableWithUnclosedEscapes();
  std::cout << "All formatter tests passed successfully.\n";
  return 0;
}
