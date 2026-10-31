#pragma once

#include <QtCore/qcontainerfwd.h>
#include <QtCore/qobject.h>
#include <QtCore/qpointer.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qtablewidget.h>
#include <QtWidgets/qwidget.h>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vinput::script {
struct ResourceVersionInfo;
}

namespace vinput::gui {

// Check if a background worker cancellation token has been signalled.
[[nodiscard]] inline bool IsWorkerCancelled(const std::atomic<bool>* cancel_token) noexcept {
  return cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed);
}

// Execute adapter start or stop asynchronously in QThreadPool and notify callback on context's
// thread.
void RunAdapterControlAsync(QWidget* context, std::string adapter_id, bool start,
                            std::function<void(bool ok, const std::string& err)> callback);

// Execute an asynchronous action while temporarily disabling buttons, re-enabling them upon
// completion and showing a warning dialog on failure.
template <typename Invoker, typename SuccessCallback>
void RunGuardedAsyncAction(QWidget* parent, const std::vector<QPushButton*>& buttons,
                           Invoker&& invoker, SuccessCallback&& on_success,
                           const QString& error_title = {}) {
  std::vector<QPointer<QPushButton>> guarded_buttons;
  guarded_buttons.reserve(buttons.size());
  for (auto* btn : buttons) {
    if (btn != nullptr) {
      btn->setEnabled(false);
      guarded_buttons.emplace_back(btn);
    }
  }

  const QPointer<QWidget> guard_parent(parent);
  std::forward<Invoker>(invoker)([guard_parent, guarded_buttons = std::move(guarded_buttons),
                                  on_success = std::forward<SuccessCallback>(on_success),
                                  error_title](bool ok, const std::string& err) mutable {
    if (!guard_parent) {
      return;
    }
    for (const auto& btn : guarded_buttons) {
      if (btn) {
        btn->setEnabled(true);
      }
    }
    if (!ok) {
      QMessageBox::warning(guard_parent, error_title.isEmpty() ? QObject::tr("Error") : error_title,
                           QString::fromStdString(err));
      return;
    }
    on_success();
  });
}

// Convenience wrapper for adapter control guarded by button states and error dialog.
template <typename SuccessCallback>
inline void RunAdapterControlGuarded(QWidget* parent, const std::vector<QPushButton*>& buttons,
                                     std::string adapter_id, bool start,
                                     SuccessCallback&& on_success,
                                     const QString& error_title = {}) {
  RunGuardedAsyncAction(
      parent, buttons,
      [parent, id = std::move(adapter_id), start](auto&& cb) {
        RunAdapterControlAsync(parent, std::move(id), start, std::forward<decltype(cb)>(cb));
      },
      std::forward<SuccessCallback>(on_success), error_title);
}

// Format user-facing status string for a registry resource (provider or adapter).
QString FormatScriptUpdateStatus(const vinput::script::ResourceVersionInfo& ver_info,
                                 bool installed);

// Split text into non-empty trimmed lines.
QStringList NonEmptyLines(const QString& text);

// Create a read-only table cell, optionally with UserRole data.
QTableWidgetItem* MakeCell(const QString& text, const QString& data = {});

// Validate LLM provider input (name + base_url).
bool ValidateProviderInput(const QString& name, const QString& base_url, QString* error_out);

// Parse KEY=VALUE lines into a map. Returns false on error.
bool ParseCommandEnv(const QString& text, std::map<std::string, std::string>* env,
                     QString* error_out);

// Populate provider combo from `vinput -j llm list`, wire it to refresh
// model combo via OpenAI /models endpoint.
void SetupProviderModelCombos(QComboBox* comboProvider, QComboBox* comboModel,
                              const QString& currentProvider = {},
                              const QString& currentModel = {});

} // namespace vinput::gui
