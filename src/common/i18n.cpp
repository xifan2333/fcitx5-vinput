#include "common/i18n.h"

#include <algorithm>
#include <array>
extern "C" {
#include <locale.h>
}
#include <cstdlib>
#include <filesystem>
#include <libintl.h>
#include <string>
#include <system_error>
#include <vector>

#include "common/utils/locale_utils.h"

#include "config.h"

namespace fs = std::filesystem;

namespace vinput::i18n {

namespace {

std::vector<std::string> LocaleCandidates() {
  std::vector<std::string> candidates;
  const auto add_candidate = [&candidates](const char* value) {
    if (value == nullptr || *value == '\0') {
      return;
    }

    const std::string locale = vinput::locale::NormalizeLocaleName(value);
    if (locale.empty()) {
      return;
    }
    if (std::ranges::find(candidates, locale) == candidates.end()) {
      candidates.push_back(locale);
    }
    const auto underscore = locale.find('_');
    if (underscore != std::string::npos) {
      const auto language = locale.substr(0, underscore);
      if (!language.empty() && std::ranges::find(candidates, language) == candidates.end()) {
        candidates.push_back(language);
      }
    }
  };

  add_candidate(std::getenv("LANGUAGE"));
  add_candidate(std::getenv("LC_ALL"));
  add_candidate(std::getenv("LC_MESSAGES"));
  add_candidate(std::getenv("LANG"));
  add_candidate(setlocale(LC_MESSAGES, nullptr));
  return candidates;
}

std::string PreferredMessageLocale() {
  constexpr std::array<const char*, 4> kLocaleEnvVars = {"LANGUAGE", "LC_ALL", "LC_MESSAGES",
                                                         "LANG"};
  for (const char* name : kLocaleEnvVars) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
      continue;
    }
    const std::string locale = vinput::locale::NormalizeLocaleName(value);
    if (!locale.empty()) {
      return locale;
    }
  }
  return {};
}

void ApplyPreferredMessageLocale() {
  const std::string preferred_locale = PreferredMessageLocale();
  if (preferred_locale.empty()) {
    return;
  }

  const std::array<std::string, 3> candidates = {
      preferred_locale,
      preferred_locale + ".UTF-8",
      preferred_locale + ".utf8",
  };
  for (const auto& candidate : candidates) {
    if (setlocale(LC_MESSAGES, candidate.c_str()) != nullptr) {
      return;
    }
  }
}

const char* ResolveLocaleDir() {
  const fs::path build_locale_dir = VINPUT_BUILD_LOCALEDIR;
  std::error_code ec;
  if (fs::exists(build_locale_dir, ec) && !ec) {
    for (const auto& locale : LocaleCandidates()) {
      const auto mo_path = build_locale_dir / locale / "LC_MESSAGES" / "fcitx5-vinput.mo";
      if (fs::exists(mo_path, ec) && !ec) {
        return VINPUT_BUILD_LOCALEDIR;
      }
    }
  }
  return VINPUT_LOCALEDIR;
}

} // namespace

void Init() {
  setlocale(LC_ALL, "");
  ApplyPreferredMessageLocale();
  bindtextdomain("fcitx5-vinput", ResolveLocaleDir());
  bind_textdomain_codeset("fcitx5-vinput", "UTF-8");
  textdomain("fcitx5-vinput");
}

} // namespace vinput::i18n
