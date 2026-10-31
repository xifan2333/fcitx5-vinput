#pragma once

#include <QtWidgets/qwidget.h>

#include "common/config/core_config_types.h"

namespace vinput::gui {

using AdapterData = ::LlmAdapter;

// Show a dialog to edit an adapter's command/args/env.
// Returns true if accepted, fills out_data.
bool ShowAdapterDialog(QWidget* parent, const AdapterData& initial, AdapterData* out_data);

} // namespace vinput::gui
