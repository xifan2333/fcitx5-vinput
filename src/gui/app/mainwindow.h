#pragma once

#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qtmetamacros.h>
#include <QtWidgets/qmainwindow.h>
#include <QtWidgets/qtabwidget.h>
#include <QtWidgets/qwidget.h>
#include <cstdint>
#include <optional>

#include "config.h"

class QPushButton;

namespace vinput::gui {
class ControlPage;
class ResourcePage;
class LlmPage;
#if VINPUT_ENABLE_LOCAL_ASR
class HotwordPage;
#endif
} // namespace vinput::gui

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  enum class UpdateCheckResult : std::uint8_t {
    NetworkError,
    ParseError,
    UpToDate,
    UpdateAvailable,
  };

  struct UpdatePayload {
    QString id;
    QString version;
    QString title;
    QString text;
    QString url;
  };

  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;
  MainWindow(MainWindow&&) = delete;
  MainWindow& operator=(MainWindow&&) = delete;

private:
  void onSaveClicked();
  static void onOpenConfigClicked();
  void onCheckUpdateClicked();
  void resetCheckUpdateButton();
  void onManualCheckCompleted(UpdateCheckResult result,
                              const std::optional<UpdatePayload>& payload = std::nullopt);
  void reloadAll();
  void onNotificationReady(const QString& id, const QString& title, const QString& text,
                           const QString& url);

  void checkNotification(bool manual = false);

  QTabWidget* tabWidget_;
  QPushButton* btnCheckUpdate_{nullptr};
  bool checking_update_{false};

  vinput::gui::ControlPage* controlPage_;
  vinput::gui::ResourcePage* resourcePage_;
  vinput::gui::LlmPage* llmPage_;
#if VINPUT_ENABLE_LOCAL_ASR
  vinput::gui::HotwordPage* hotwordPage_;
#endif
};
