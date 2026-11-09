#pragma once

#include <QtCore/qstring.h>
#include <QtWidgets/qwidget.h>

#include "common/config/core_config_types.h"

namespace vinput::gui {

// Show a dialog to edit a client's (trigger or receiver) command, args, env, and autoStart.
// Returns true if accepted, fills out_data.
bool ShowClientDialog(QWidget* parent, const QString& title, const ClientSpec& initial,
                      ClientSpec* out_data);

} // namespace vinput::gui
