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
  [[nodiscard]] QString hotwordsFilePath() const;
  [[nodiscard]] QString hotwordsContent() const;

private:
  void onBrowseClicked();

  QLineEdit* editFile_;
  QTextEdit* textContent_;
  QPushButton* btnBrowse_;
};

} // namespace vinput::gui
