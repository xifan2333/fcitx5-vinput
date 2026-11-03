#include "pages/hotwords/hotword_page.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <variant>

#include "common/config/core_config_types.h"

#include "gui/utils/config_manager.h"

namespace vinput::gui {

HotwordPage::HotwordPage(QWidget* parent)
    : QWidget(parent), editFile_(new QLineEdit()), btnBrowse_(new QPushButton(tr("Browse..."))),
      textContent_(new QTextEdit()) {
  auto* layout = new QVBoxLayout(this);

  auto* fileLayout = new QHBoxLayout();
  editFile_->setPlaceholderText(tr("Path to hotwords file..."));
  fileLayout->addWidget(editFile_);
  fileLayout->addWidget(btnBrowse_);
  layout->addLayout(fileLayout);

  auto* lblWords =
      new QLabel(tr("Hotwords (one per line, optional transducer score: \"word:2.0\"):"));
  layout->addWidget(lblWords);

  layout->addWidget(textContent_);

  connect(btnBrowse_, &QPushButton::clicked, this, &HotwordPage::onBrowseClicked);
  connect(editFile_, &QLineEdit::editingFinished, this, [this]() {
    const QString path = editFile_->text().trimmed();
    if (path.isEmpty()) {
      textContent_->clear();
      return;
    }
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
      textContent_->setPlainText(QTextStream(&f).readAll());
    }
  });

  QTimer::singleShot(0, this, &HotwordPage::reload);
}

void HotwordPage::reload() {
  const CoreConfig config = vinput::gui::ConfigManager::Get().Load();
  QString path;
  for (const auto& prov : config.asr.providers) {
    if (const auto* local = std::get_if<LocalAsrProvider>(&prov)) {
      path = QString::fromStdString(local->hotwordsFile);
      break;
    }
  }
  editFile_->setText(path);
  if (!path.isEmpty()) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
      textContent_->setPlainText(QTextStream(&f).readAll());
    }
  } else {
    textContent_->clear();
  }
}

QString HotwordPage::hotwordsFilePath() const {
  return editFile_->text().trimmed();
}

QString HotwordPage::hotwordsContent() const {
  return textContent_->toPlainText();
}

void HotwordPage::onBrowseClicked() {
  const QString fileName = QFileDialog::getOpenFileName(this, tr("Select Hotwords File"), "",
                                                        tr("Text Files (*.txt);;All Files (*)"));
  if (fileName.isEmpty()) {
    return;
  }
  editFile_->setText(fileName);
  QFile f(fileName);
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    textContent_->setPlainText(QTextStream(&f).readAll());
  }
}

} // namespace vinput::gui
