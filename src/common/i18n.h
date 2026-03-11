#pragma once

#include <libintl.h>

namespace vinput::i18n {
void Init();
}

#ifndef _
#define _(String) gettext(String)
#endif
