#include "common/i18n.h"

#include "config.h"

#include <locale.h>

namespace vinput::i18n {

void Init() {
  setlocale(LC_ALL, "");
  bindtextdomain("fcitx5-vinput", VINPUT_LOCALEDIR);
  bind_textdomain_codeset("fcitx5-vinput", "UTF-8");
  textdomain("fcitx5-vinput");
}

} // namespace vinput::i18n
