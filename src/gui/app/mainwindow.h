#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>

#include "config.h"

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
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;
  MainWindow(MainWindow&&) = delete;
  MainWindow& operator=(MainWindow&&) = delete;

private:
  void onSaveClicked();
  static void onOpenConfigClicked();
  void reloadAll();
  void onNotificationReady(const QString& id, const QString& title, const QString& text,
                           const QString& url);

  void checkNotification();

  QTabWidget* tabWidget_;

  vinput::gui::ControlPage* controlPage_;
  vinput::gui::ResourcePage* resourcePage_;
  vinput::gui::LlmPage* llmPage_;
#if VINPUT_ENABLE_LOCAL_ASR
  vinput::gui::HotwordPage* hotwordPage_;
#endif
};
