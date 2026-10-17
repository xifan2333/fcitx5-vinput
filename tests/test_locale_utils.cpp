#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/utils/locale_utils.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestNormalizeLocale() {
  using vinput::locale::NormalizeLocale;
  using vinput::locale::NormalizeLocaleName;

  // Basic normalization
  TEST_ASSERT(NormalizeLocale("zh_CN.UTF-8") == "zh_CN");
  TEST_ASSERT(NormalizeLocale("en-US@euro") == "en_US");
  TEST_ASSERT(NormalizeLocale("de_DE:de") == "de_DE");
  TEST_ASSERT(NormalizeLocale("zh-CN.gb2312@pinyin:zh") == "zh_CN");

  // Reserved or invalid system locales
  TEST_ASSERT(NormalizeLocale("").empty());
  TEST_ASSERT(NormalizeLocale("C").empty());
  TEST_ASSERT(NormalizeLocale("POSIX").empty());
  TEST_ASSERT(NormalizeLocale("C.UTF-8").empty());
  TEST_ASSERT(NormalizeLocale("POSIX@latin").empty());
  TEST_ASSERT(NormalizeLocale(":invalid").empty());
  TEST_ASSERT(NormalizeLocale("@modifier").empty());
  TEST_ASSERT(NormalizeLocale(".encoding").empty());

  // Hyphen replacement option
  TEST_ASSERT(NormalizeLocale("zh-CN", /*replace_hyphen=*/false) == "zh-CN");
  TEST_ASSERT(NormalizeLocale("zh-CN", /*replace_hyphen=*/true) == "zh_CN");
  TEST_ASSERT(NormalizeLocale("en-US-variant", /*replace_hyphen=*/false) == "en-US-variant");
  TEST_ASSERT(NormalizeLocale("en-US-variant", /*replace_hyphen=*/true) == "en_US_variant");

  // NormalizeLocaleName alias (preserves hyphens)
  TEST_ASSERT(NormalizeLocaleName("zh-CN") == "zh-CN");
  TEST_ASSERT(NormalizeLocaleName("zh-CN.UTF-8") == "zh-CN");
  TEST_ASSERT(NormalizeLocaleName("en-US@calendar") == "en-US");
  TEST_ASSERT(NormalizeLocaleName("C").empty());
  TEST_ASSERT(NormalizeLocaleName("").empty());
}

void TestCanonicalizeLocale() {
  using vinput::locale::CanonicalizeLocale;

  // Dialect expansion when lacking region
  TEST_ASSERT(CanonicalizeLocale("zh") == "zh_CN");
  TEST_ASSERT(CanonicalizeLocale("zh.UTF-8") == "zh_CN");
  TEST_ASSERT(CanonicalizeLocale("en") == "en_US");
  TEST_ASSERT(CanonicalizeLocale("en.UTF-8") == "en_US");

  // Preserves existing region
  TEST_ASSERT(CanonicalizeLocale("zh_CN") == "zh_CN");
  TEST_ASSERT(CanonicalizeLocale("zh_TW") == "zh_TW");
  TEST_ASSERT(CanonicalizeLocale("zh-TW") == "zh_TW");
  TEST_ASSERT(CanonicalizeLocale("en_GB") == "en_GB");
  TEST_ASSERT(CanonicalizeLocale("en-US") == "en_US");

  // Non-expanded locales without underscore
  TEST_ASSERT(CanonicalizeLocale("ja") == "ja");
  TEST_ASSERT(CanonicalizeLocale("fr") == "fr");
  TEST_ASSERT(CanonicalizeLocale("de") == "de");

  // Edge cases
  TEST_ASSERT(CanonicalizeLocale("").empty());
  TEST_ASSERT(CanonicalizeLocale("C").empty());
  TEST_ASSERT(CanonicalizeLocale("POSIX").empty());
  TEST_ASSERT(CanonicalizeLocale("C.UTF-8").empty());
}

void TestExtractLanguageCode() {
  using vinput::locale::ExtractLanguageCode;

  TEST_ASSERT(ExtractLanguageCode("zh_CN") == "zh");
  TEST_ASSERT(ExtractLanguageCode("en-US") == "en");
  TEST_ASSERT(ExtractLanguageCode("fr") == "fr");
  TEST_ASSERT(ExtractLanguageCode("").empty());
  TEST_ASSERT(ExtractLanguageCode("-CN").empty());
  TEST_ASSERT(ExtractLanguageCode("_CN").empty());
  TEST_ASSERT(ExtractLanguageCode("zh_Hant_TW") == "zh");
  TEST_ASSERT(ExtractLanguageCode("zh-Hans-CN") == "zh");

  // Zero-copy check: returned string_view points to input buffer
  const std::string_view input = "zh_CN";
  const std::string_view lang = ExtractLanguageCode(input);
  TEST_ASSERT(lang.data() == input.data());
  TEST_ASSERT(lang.size() == 2);
}

int main() {
  TestNormalizeLocale();
  TestCanonicalizeLocale();
  TestExtractLanguageCode();

  std::printf("All test_locale_utils tests passed!\n");
  return 0;
}
