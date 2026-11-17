#pragma once

#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTextEdit>
#include <QWidget>

namespace vinput::gui {

class HotwordPage : public QWidget {
  Q_OBJECT

public:
  explicit HotwordPage(QWidget* parent = nullptr);

  void reload();

  // Accessors for MainWindow save.
  QString hotwordsFilePath() const;
  QString hotwordsContent() const;

private slots:
  void onBrowseClicked();

private:
  QLineEdit* editFile_;
  QTextEdit* textContent_;
  QPushButton* btnBrowse_;
};

} // namespace vinput::gui
