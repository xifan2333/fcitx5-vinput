#pragma once

#include <QMainWindow>
#include <QTabWidget>

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

private slots:
  void onSaveClicked();
  void onOpenConfigClicked();
  void reloadAll();
  void onNotificationReady(QString id, QString title, QString text, QString url);

private:
  void checkNotification();

  QTabWidget* tabWidget_;

  vinput::gui::ControlPage* controlPage_;
  vinput::gui::ResourcePage* resourcePage_;
  vinput::gui::LlmPage* llmPage_;
#if VINPUT_ENABLE_LOCAL_ASR
  vinput::gui::HotwordPage* hotwordPage_;
#endif
};
