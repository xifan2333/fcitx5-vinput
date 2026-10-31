#pragma once

#include <QtCore/qobject.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qtmetamacros.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qwidget.h>

namespace vinput::gui {

class LlmPage : public QWidget {
  Q_OBJECT

public:
  explicit LlmPage(QWidget* parent = nullptr);

  void reload();

signals:
  void configChanged();

private slots:
  void refreshLlmList();
  void refreshAdapterList();
  void refreshSceneList();
  void onLlmAdd();
  void onLlmEdit();
  void onLlmRemove();
  void onLlmTest();
  void onAdapterEdit();
  void onAdapterRemove();
  void onAdapterStart();
  void onAdapterStop();
  void updateAdapterButtons();
  void onSceneAdd();
  void onSceneEdit();
  void onSceneRemove();
  void onSceneSetActive();

  void executeAdapterControl(bool start);

  QListWidget* listProviders_;
  QPushButton* btnLlmAdd_;
  QPushButton* btnLlmEdit_;
  QPushButton* btnLlmRemove_;
  QPushButton* btnLlmTest_;

  QListWidget* listAdapters_;
  QPushButton* btnAdapterEdit_;
  QPushButton* btnAdapterRemove_;
  QPushButton* btnAdapterStart_;
  QPushButton* btnAdapterStop_;
  QPushButton* btnAdapterRefresh_;

  QListWidget* listScenes_;
  QPushButton* btnSceneAdd_;
  QPushButton* btnSceneEdit_;
  QPushButton* btnSceneRemove_;
  QPushButton* btnSceneSetActive_;
};

} // namespace vinput::gui
