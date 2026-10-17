#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vinput::locale {

/**
 * @brief 提取语言代码前缀（如 "zh_CN" -> "zh", "en_US" -> "en", "ja" -> "ja"）。
 *
 * @param locale 输入的语言或区域标识符（支持已清洗或带下划线/连字符的形式）
 * @return 截取分隔符之前的语言代码；若未包含分隔符则返回原字符串
 */
inline std::string_view ExtractLanguageCode(std::string_view locale) noexcept {
  const size_t sep = locale.find_first_of("-_");
  if (sep != std::string_view::npos) {
    return locale.substr(0, sep);
  }
  return locale;
}

/**
 * @brief 基础清洗 Locale 标识符：剥离修饰符与特殊编码。
 *
 * 裁剪逻辑：
 * 1. 截断 ':' 后的路径或修饰部分；
 * 2. 截断 '.' 后的字符集编码（如 .UTF-8）；
 * 3. 截断 '@' 后的修饰符（如 @latin, @pinyin）；
 * 4. 可选将连字符 '-' 替换为下划线 '_'（统一格式）；
 * 5. 过滤 "C"、"POSIX" 等非自然语言标识，均返回空字符串。
 *
 * @param raw_locale 原始输入（例如 "zh_CN.UTF-8", "en-US@calendar", "C", "POSIX"）
 * @param replace_hyphen 是否将 '-' 统一替换为 '_'
 * @return 清洗后的合法语言标签；若无效或为系统保留值则返回空字符串
 */
inline std::string NormalizeLocale(std::string_view raw_locale, bool replace_hyphen = true) {
  if (raw_locale.empty()) {
    return {};
  }
  std::string_view result = raw_locale;
  const size_t colon = result.find(':');
  if (colon != std::string_view::npos) {
    result = result.substr(0, colon);
  }
  const size_t dot = result.find('.');
  if (dot != std::string_view::npos) {
    result = result.substr(0, dot);
  }
  const size_t at = result.find('@');
  if (at != std::string_view::npos) {
    result = result.substr(0, at);
  }

  if (result.empty() || result == "C" || result == "POSIX") {
    return {};
  }

  std::string normalized(result);
  if (replace_hyphen) {
    for (char& ch : normalized) {
      if (ch == '-') {
        ch = '_';
      }
    }
  }
  return normalized;
}

/**
 * @brief 规范化 Locale 标识符（保留连字符 '-'，主要用于系统候选匹配）。
 *
 * 等价于 NormalizeLocale(raw_locale, false)。
 *
 * @param raw_locale 原始输入
 * @return 清洗后的合法语言标签（保留原有 '-' 分隔符）
 */
inline std::string NormalizeLocaleName(std::string_view raw_locale) {
  return NormalizeLocale(raw_locale, /*replace_hyphen=*/false);
}

/**
 * @brief 标准化并在缺乏区域（Region）时补充默认方言别名映射。
 *
 * 规则：在 NormalizeLocale 基础上，若结果无下划线：
 * - "zh" -> "zh_CN"
 * - "en" -> "en_US"
 *
 * @param raw_locale 原始输入
 * @return 带有默认方言倾向的标准 Locale 标签
 */
inline std::string CanonicalizeLocale(std::string_view raw_locale) {
  std::string locale = NormalizeLocale(raw_locale, /*replace_hyphen=*/true);
  if (locale.empty()) {
    return {};
  }
  if (locale.find('_') == std::string_view::npos) {
    if (locale == "zh") {
      return "zh_CN";
    }
    if (locale == "en") {
      return "en_US";
    }
  }
  return locale;
}

} // namespace vinput::locale
