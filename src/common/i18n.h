#pragma once

#include <libintl.h> // IWYU pragma: export

namespace vinput::i18n {
void Init();
}

#ifndef _
#define _(String) gettext(String)
#endif
