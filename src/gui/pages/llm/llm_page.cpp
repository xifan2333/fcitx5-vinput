#include "pages/llm/llm_page.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "dialogs/adapter_dialog.h"
#include "utils/gui_helpers.h"
#include "gui/utils/config_manager.h"
#include "gui/utils/i18n_cache.h"
#include "cli/runtime/dbus_client.h"
#include "common/llm/defaults.h"
#include "common/scene/postprocess_scene.h"
#include "common/llm/adapter_manager.h"
#include "common/registry/registry_i18n.h"

namespace vinput::gui {

namespace {

vinput::scene::Config ToSceneConfig(const CoreConfig::Scenes& sc) {
  vinput::scene::Config c;
  c.activeSceneId = sc.activeScene;
  c.scenes = sc.definitions;
  return c;
}

void FromSceneConfig(CoreConfig::Scenes& sc, const vinput::scene::Config& c) {
  sc.activeScene = c.activeSceneId;
  sc.definitions = c.scenes;
}

// Parses the Extra body JSON text from a dialog. Returns true with `out` set
// on success (or empty-object when text is blank); false after showing a
// QMessageBox on parse error or non-object payloads.
bool ParseExtraBodyInput(QWidget *parent, const QString &text,
                         nlohmann::json *out) {
  const std::string trimmed = text.trimmed().toStdString();
  if (trimmed.empty()) {
    *out = nlohmann::json::object();
    return true;
  }
  try {
    auto parsed = nlohmann::json::parse(trimmed);
    if (!parsed.is_object()) {
      QMessageBox::warning(
          parent, LlmPage::tr("Error"),
          LlmPage::tr("Extra body must be a JSON object (e.g. {\"enable_thinking\": false})."));
      return false;
    }
    *out = std::move(parsed);
    return true;
  } catch (const std::exception &e) {
    QMessageBox::warning(
        parent, LlmPage::tr("Error"),
        LlmPage::tr("Invalid extra_body JSON: %1").arg(QString::fromUtf8(e.what())));
    return false;
  }
}

QPlainTextEdit *MakeExtraBodyEditor(const nlohmann::json *prefill = nullptr) {
  auto *edit = new QPlainTextEdit();
  edit->setPlaceholderText(
      LlmPage::tr("Optional JSON object merged into each request body, e.g. "
                  "{\"enable_thinking\": false}"));
  edit->setTabChangesFocus(true);
  const QFontMetrics fm(edit->font());
  edit->setFixedHeight(fm.lineSpacing() * 5 + 8);
  if (prefill && prefill->is_object() && !prefill->empty()) {
    edit->setPlainText(QString::fromStdString(prefill->dump(2)));
  }
  return edit;
}

constexpr int kDefaultTimeoutMs = vinput::scene::kDefaultTimeoutMs;
constexpr int kDefaultCandidateCount = 3;
constexpr int kMinCandidateCount = 1;
constexpr int kMaxCandidateCount = 10;

QString SceneLabelForGui(const vinput::scene::Definition &scene) {
  if (scene.id == vinput::scene::kRawSceneId || scene.label == vinput::scene::kRawSceneLabelKey)
    return GuiTranslate("Raw");
  if (scene.id == vinput::scene::kCommandSceneId || scene.label == vinput::scene::kCommandSceneLabelKey)
    return GuiTranslate("Command");
  if (!scene.label.empty())
    return QString::fromStdString(scene.label);
  return QString::fromStdString(scene.id);
}

// Convert between AdapterData (GUI dialog) and LlmAdapter (config).
AdapterData AdapterDataFromConfig(const LlmAdapter &a) {
  return {a.id, a.command, a.args, a.env};
}

LlmAdapter LlmAdapterFromDialog(const AdapterData &d) {
  return {d.id, d.command, d.args, d.env};
}

template <typename Callback>
void RunAdapterControlAsync(LlmPage *page, std::string adapter_id,
                            bool start, Callback callback) {
  QPointer<LlmPage> self(page);
  QThreadPool::globalInstance()->start(
      [self, adapter_id = std::move(adapter_id), start,
       callback = std::move(callback)]() mutable {
        vinput::cli::DbusClient dbus;
        std::string err;
        const bool ok = start ? dbus.StartAdapter(adapter_id, &err)
                              : dbus.StopAdapter(adapter_id, &err);
        QMetaObject::invokeMethod(
            self, [self, callback = std::move(callback), ok, err]() mutable {
              if (!self) {
                return;
              }
              callback(ok, err);
            });
      });
}

}  // namespace

LlmPage::LlmPage(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(tr("<b>Providers</b>")));
  auto *listLayout = new QHBoxLayout();
  listProviders_ = new QListWidget();
  listLayout->addWidget(listProviders_);

  auto *btnLayout = new QVBoxLayout();
  btnLlmAdd_ = new QPushButton(tr("Add"));
  btnLlmEdit_ = new QPushButton(tr("Edit"));
  btnLlmRemove_ = new QPushButton(tr("Remove"));
  btnLlmTest_ = new QPushButton(tr("Test"));
  btnLayout->addWidget(btnLlmAdd_);
  btnLayout->addWidget(btnLlmEdit_);
  btnLayout->addWidget(btnLlmRemove_);
  btnLayout->addWidget(btnLlmTest_);
  btnLayout->addStretch();
  listLayout->addLayout(btnLayout);
  layout->addLayout(listLayout);

  auto *hint = new QLabel(tr(
      "LLM adapters are local OpenAI-compatible bridge processes. Install them "
      "from the Resources page, then manage runtime here."));
  hint->setWordWrap(true);
  layout->addWidget(hint);

  auto *adapterLayout = new QHBoxLayout();
  listAdapters_ = new QListWidget();
  adapterLayout->addWidget(listAdapters_);

  auto *adapterBtnLayout = new QVBoxLayout();
  btnAdapterEdit_ = new QPushButton(tr("Edit"));
  btnAdapterStart_ = new QPushButton(tr("Start"));
  btnAdapterStop_ = new QPushButton(tr("Stop"));
  btnAdapterRefresh_ = new QPushButton(tr("Refresh"));
  adapterBtnLayout->addWidget(btnAdapterEdit_);
  adapterBtnLayout->addWidget(btnAdapterStart_);
  adapterBtnLayout->addWidget(btnAdapterStop_);
  adapterBtnLayout->addWidget(btnAdapterRefresh_);
  adapterBtnLayout->addStretch();
  adapterLayout->addLayout(adapterBtnLayout);

  layout->addWidget(new QLabel(tr("<b>Installed Adapters</b>")));
  layout->addLayout(adapterLayout);

  // Scenes section
  layout->addWidget(new QLabel(tr("<b>Scenes</b>")));
  auto *sceneLayout = new QHBoxLayout();
  listScenes_ = new QListWidget();
  sceneLayout->addWidget(listScenes_);

  auto *sceneBtnLayout = new QVBoxLayout();
  btnSceneAdd_ = new QPushButton(tr("Add"));
  btnSceneEdit_ = new QPushButton(tr("Edit"));
  btnSceneRemove_ = new QPushButton(tr("Remove"));
  btnSceneSetActive_ = new QPushButton(tr("Activate"));
  sceneBtnLayout->addWidget(btnSceneAdd_);
  sceneBtnLayout->addWidget(btnSceneEdit_);
  sceneBtnLayout->addWidget(btnSceneRemove_);
  sceneBtnLayout->addWidget(btnSceneSetActive_);
  sceneBtnLayout->addStretch();
  sceneLayout->addLayout(sceneBtnLayout);
  layout->addLayout(sceneLayout);

  connect(btnLlmAdd_, &QPushButton::clicked, this, &LlmPage::onLlmAdd);
  connect(btnLlmEdit_, &QPushButton::clicked, this, &LlmPage::onLlmEdit);
  connect(btnLlmRemove_, &QPushButton::clicked, this, &LlmPage::onLlmRemove);
  connect(btnLlmTest_, &QPushButton::clicked, this, &LlmPage::onLlmTest);
  connect(btnAdapterEdit_, &QPushButton::clicked, this,
          &LlmPage::onAdapterEdit);
  connect(btnAdapterStart_, &QPushButton::clicked, this,
          &LlmPage::onAdapterStart);
  connect(btnAdapterStop_, &QPushButton::clicked, this,
          &LlmPage::onAdapterStop);
  connect(btnAdapterRefresh_, &QPushButton::clicked, this,
          &LlmPage::refreshAdapterList);
  connect(btnSceneAdd_, &QPushButton::clicked, this, &LlmPage::onSceneAdd);
  connect(btnSceneEdit_, &QPushButton::clicked, this, &LlmPage::onSceneEdit);
  connect(btnSceneRemove_, &QPushButton::clicked, this,
          &LlmPage::onSceneRemove);
  connect(btnSceneSetActive_, &QPushButton::clicked, this,
          &LlmPage::onSceneSetActive);

  QTimer::singleShot(0, this, &LlmPage::refreshAdapterList);
  QTimer::singleShot(0, this, &LlmPage::refreshLlmList);
  QTimer::singleShot(0, this, &LlmPage::refreshSceneList);
}

void LlmPage::reload() {
  refreshLlmList();
  refreshAdapterList();
  refreshSceneList();
}

void LlmPage::refreshLlmList() {
  listProviders_->clear();
  CoreConfig config = ConfigManager::Get().Load();

  for (const auto &provider : config.llm.providers) {
    QString name = QString::fromStdString(provider.id);
    QString base_url = QString::fromStdString(provider.base_url);
    QString display = QString("%1 @ %2").arg(name, base_url);

    auto *item = new QListWidgetItem(display, listProviders_);
    item->setData(Qt::UserRole, name);
  }
}

void LlmPage::refreshAdapterList() {
  listAdapters_->clear();
  CoreConfig config = ConfigManager::Get().Load();

  for (const auto &adapter : config.llm.adapters) {
    QString id = QString::fromStdString(adapter.id);
    bool running = vinput::adapter::IsRunning(adapter.id);

    QString display = id + " · " +
                      (running ? GuiTranslate("running")
                               : GuiTranslate("stopped"));
    QString command = QString::fromStdString(adapter.command);
    if (!command.isEmpty()) {
      display += " · " + command;
    }

    auto *item = new QListWidgetItem(display, listAdapters_);
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, running);

    // Build tooltip
    QString tooltip;
    if (!command.isEmpty()) {
      tooltip += "\n" + tr("Command: %1").arg(command);
    }
    if (!adapter.args.empty()) {
      QStringList argsList;
      for (const auto &a : adapter.args)
        argsList << QString::fromStdString(a);
      tooltip += "\n" + tr("Args: %1").arg(argsList.join(" "));
    }
    if (!adapter.env.empty()) {
      QStringList envList;
      for (const auto &[k, v] : adapter.env)
        envList << QString::fromStdString(k) + "=" + QString::fromStdString(v);
      tooltip += "\n" + tr("Env: %1").arg(envList.join(" "));
    }
    item->setToolTip(tooltip.trimmed());
  }
}

void LlmPage::onLlmAdd() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Add LLM Provider"));

  auto *form = new QFormLayout();
  auto *editName = new QLineEdit();
  auto *editBaseUrl = new QLineEdit();
  auto *editApiKey = new QLineEdit();
  editApiKey->setEchoMode(QLineEdit::Password);
  auto *editExtraBody = MakeExtraBodyEditor();

  form->addRow(tr("Name:"), editName);
  form->addRow(tr("Base URL:"), editBaseUrl);
  form->addRow(tr("API Key:"), editApiKey);
  form->addRow(tr("Extra body (JSON):"), editExtraBody);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  const QString name_text = editName->text().trimmed();
  const QString base_url_text = editBaseUrl->text().trimmed();
  QString validation_error;
  if (!ValidateProviderInput(name_text, base_url_text, &validation_error)) {
    QMessageBox::warning(this, tr("Error"), validation_error);
    return;
  }

  nlohmann::json extra_body;
  if (!ParseExtraBodyInput(this, editExtraBody->toPlainText(), &extra_body)) {
    return;
  }

  CoreConfig config = ConfigManager::Get().Load();
  if (ResolveLlmProvider(config, name_text.toStdString()) != nullptr) {
    QMessageBox::warning(this, tr("Error"), tr("LLM provider '%1' already exists.").arg(name_text));
    return;
  }

  LlmProvider provider;
  provider.id = name_text.toStdString();
  provider.base_url = base_url_text.toStdString();
  provider.api_key = editApiKey->text().trimmed().toStdString();
  provider.extra_body = std::move(extra_body);
  config.llm.providers.push_back(std::move(provider));

  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshLlmList();
  emit configChanged();
}

void LlmPage::onLlmEdit() {
  auto *item = listProviders_->currentItem();
  if (!item)
    return;

  QString provider_name = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  const LlmProvider* prov = ResolveLlmProvider(config, provider_name.toStdString());
  if (!prov) return;

  QString current_base_url = QString::fromStdString(prov->base_url);

  QDialog dialog(this);
  dialog.setWindowTitle(tr("Edit LLM Provider"));

  auto *form = new QFormLayout();
  auto *editName = new QLineEdit(provider_name);
  editName->setReadOnly(true);
  auto *editBaseUrl = new QLineEdit(current_base_url);
  auto *editApiKey = new QLineEdit();
  editApiKey->setEchoMode(QLineEdit::Password);
  editApiKey->setPlaceholderText(tr("Leave empty to keep current key"));
  auto *editExtraBody = MakeExtraBodyEditor(&prov->extra_body);

  form->addRow(tr("Name:"), editName);
  form->addRow(tr("Base URL:"), editBaseUrl);
  form->addRow(tr("API Key:"), editApiKey);
  form->addRow(tr("Extra body (JSON):"), editExtraBody);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  const QString base_url_text = editBaseUrl->text().trimmed();
  QString validation_error;
  if (!ValidateProviderInput(provider_name, base_url_text, &validation_error)) {
    QMessageBox::warning(this, tr("Error"), validation_error);
    return;
  }

  nlohmann::json extra_body;
  if (!ParseExtraBodyInput(this, editExtraBody->toPlainText(), &extra_body)) {
    return;
  }

  auto &providers = config.llm.providers;
  auto it = std::find_if(providers.begin(), providers.end(), [&](const LlmProvider &p) { return p.id == provider_name.toStdString(); });
  if (it != providers.end()) {
      it->base_url = base_url_text.toStdString();
      const QString api_key_text = editApiKey->text().trimmed();
      if (!api_key_text.isEmpty()) {
          it->api_key = api_key_text.toStdString();
      }
      it->extra_body = std::move(extra_body);
      if (!ConfigManager::Get().Save(config)) {
          QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
          return;
      }
  }

  refreshLlmList();
  emit configChanged();
}

void LlmPage::onLlmRemove() {
  auto *item = listProviders_->currentItem();
  if (!item)
    return;

  QString provider_name = item->data(Qt::UserRole).toString();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove LLM provider '%1'?")
          .arg(provider_name));
  if (response != QMessageBox::Yes)
    return;

  CoreConfig config = ConfigManager::Get().Load();
  auto &providers = config.llm.providers;
  auto it = std::find_if(providers.begin(), providers.end(), [&](const LlmProvider &p) { return p.id == provider_name.toStdString(); });
  int cleared_scene_refs = 0;
  if (it != providers.end()) {
      providers.erase(it);
      vinput::scene::Config sc = ToSceneConfig(config.scenes);
      cleared_scene_refs =
          vinput::scene::ClearProviderReferences(&sc, provider_name.toStdString());
      if (cleared_scene_refs > 0) {
        FromSceneConfig(config.scenes, sc);
      }
      if (!ConfigManager::Get().Save(config)) {
          QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
          return;
      }
  }

  refreshLlmList();
  refreshSceneList();
  emit configChanged();
  if (cleared_scene_refs > 0) {
    QMessageBox::information(
        this, tr("Scenes Updated"),
        tr("Removed provider '%1' and cleared it from %2 scene(s).")
            .arg(provider_name)
            .arg(cleared_scene_refs));
  }
}

void LlmPage::onLlmTest() {
  auto *item = listProviders_->currentItem();
  if (!item)
    return;

  QString provider_id = item->data(Qt::UserRole).toString();
  CoreConfig config = ConfigManager::Get().Load();
  const LlmProvider *provider = ResolveLlmProvider(config, provider_id.toStdString());
  if (!provider) {
    QMessageBox::warning(this, tr("Error"),
                         tr("Provider '%1' not found.").arg(provider_id));
    return;
  }

  QString url = QString::fromStdString(provider->base_url);
  if (url.isEmpty()) {
    QMessageBox::warning(this, tr("Error"), tr("Provider base_url is empty."));
    return;
  }
  while (!url.isEmpty() && url.endsWith(u'/')) url.chop(1);
  url += vinput::llm::kOpenAiModelsPath;

  btnLlmTest_->setEnabled(false);
  btnLlmTest_->setText(tr("Testing..."));

  auto *nam = new QNetworkAccessManager(this);
  QNetworkRequest req{QUrl(url)};
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  if (!provider->api_key.empty()) {
    QByteArray bearer = QByteArray(vinput::llm::kBearerPrefix) +
                        QByteArray::fromStdString(provider->api_key);
    req.setRawHeader(vinput::llm::kAuthorizationHeader, bearer);
  }

  QNetworkReply *reply = nam->get(req);
  auto *timeout = new QTimer(reply);
  timeout->setSingleShot(true);
  QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
    if (!reply->isFinished()) reply->abort();
  });
  timeout->start(10000);

  QObject::connect(reply, &QNetworkReply::finished, this,
      [this, reply, timeout, nam, provider_id]() {
    timeout->stop();
    btnLlmTest_->setEnabled(true);
    btnLlmTest_->setText(tr("Test"));

    if (reply->error() != QNetworkReply::NoError) {
      QMessageBox::warning(this, tr("Test Failed"),
                           tr("Connection to '%1' failed:\n%2")
                               .arg(provider_id, reply->errorString()));
    } else {
      QJsonParseError parseError;
      QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
      if (parseError.error != QJsonParseError::NoError || !doc.isObject() ||
          !doc.object().value("data").isArray()) {
        QMessageBox::warning(this, tr("Test Failed"),
                             tr("Invalid response from '%1'.").arg(provider_id));
      } else {
        QJsonArray data = doc.object().value("data").toArray();
        QStringList models;
        for (const auto &v : data) {
          QString id = v.toObject().value("id").toString();
          if (!id.isEmpty()) models.append(id);
        }
        models.sort();
        QString msg = tr("Connected to '%1'.\nFound %2 model(s).")
                          .arg(provider_id)
                          .arg(models.size());
        if (!models.isEmpty()) {
          msg += "\n\n" + models.join("\n");
        }
        QMessageBox::information(this, tr("Test Succeeded"), msg);
      }
    }
    reply->deleteLater();
    nam->deleteLater();
  });
}

void LlmPage::onAdapterEdit() {
  auto *item = listAdapters_->currentItem();
  if (!item)
    return;

  QString adapter_id = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  AdapterData current;
  bool found = false;
  int idx = -1;
  for (size_t i = 0; i < config.llm.adapters.size(); ++i) {
    if (config.llm.adapters[i].id == adapter_id.toStdString()) {
      current = AdapterDataFromConfig(config.llm.adapters[i]);
      found = true;
      idx = static_cast<int>(i);
      break;
    }
  }
  
  if (!found) {
    QMessageBox::warning(
        this, tr("Error"),
        tr("Adapter '%1' not found in configuration.").arg(adapter_id));
    return;
  }

  AdapterData updated;
  if (!ShowAdapterDialog(this, current, &updated)) {
    return;
  }

  config.llm.adapters[idx] = LlmAdapterFromDialog(updated);
  if (!ConfigManager::Get().Save(config)) {
      QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
      return;
  }

  refreshAdapterList();
  emit configChanged();
}

void LlmPage::onAdapterStart() {
  auto *item = listAdapters_->currentItem();
  if (!item)
    return;

  const QString adapter_id = item->data(Qt::UserRole).toString();
  btnAdapterStart_->setEnabled(false);
  btnAdapterStop_->setEnabled(false);
  RunAdapterControlAsync(this, adapter_id.toStdString(), true,
                         [this, adapter_id](bool ok, const std::string &err) {
                           btnAdapterStart_->setEnabled(true);
                           btnAdapterStop_->setEnabled(true);
                           if (!ok) {
                             QMessageBox::warning(
                                 this, tr("Error"),
                                 QString::fromStdString(err));
                             return;
                           }
                           QMessageBox::information(
                               this, tr("LLM Adapter Started"),
                               tr("Adapter '%1' started.").arg(adapter_id));
                           refreshAdapterList();
                         });
}

void LlmPage::onAdapterStop() {
  auto *item = listAdapters_->currentItem();
  if (!item)
    return;

  const QString adapter_id = item->data(Qt::UserRole).toString();
  btnAdapterStart_->setEnabled(false);
  btnAdapterStop_->setEnabled(false);
  RunAdapterControlAsync(this, adapter_id.toStdString(), false,
                         [this](bool ok, const std::string &err) {
                           btnAdapterStart_->setEnabled(true);
                           btnAdapterStop_->setEnabled(true);
                           if (!ok) {
                             QMessageBox::warning(
                                 this, tr("Error"),
                                 QString::fromStdString(err));
                             return;
                           }
                           refreshAdapterList();
                         });
}

void LlmPage::refreshSceneList() {
  listScenes_->clear();
  CoreConfig config = ConfigManager::Get().Load();

  for (const auto &scene : config.scenes.definitions) {
    QString label = SceneLabelForGui(scene);
    bool active = (scene.id == config.scenes.activeScene);
    if (active)
      label += " *";
    auto *item = new QListWidgetItem(label, listScenes_);
    item->setData(Qt::UserRole, QString::fromStdString(scene.id));
  }
}

void LlmPage::onSceneAdd() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Add Scene"));

  auto *form = new QFormLayout();
  auto *editId = new QLineEdit();
  auto *editLabel = new QLineEdit();
  auto *editPrompt = new QTextEdit();
  editPrompt->setMaximumHeight(100);
  auto *comboProvider = new QComboBox();
  auto *comboModel = new QComboBox();
  auto *spinTimeout = new QSpinBox();
  spinTimeout->setRange(1000, 300000);
  spinTimeout->setSingleStep(1000);
  spinTimeout->setValue(kDefaultTimeoutMs);
  spinTimeout->setSuffix(" ms");
  auto *spinCandidates = new QSpinBox();
  spinCandidates->setRange(kMinCandidateCount, kMaxCandidateCount);
  spinCandidates->setValue(kDefaultCandidateCount);
  auto *spinContextLines = new QSpinBox();
  spinContextLines->setRange(0, 9999);
  spinContextLines->setValue(vinput::scene::kDefaultContextLines);

  SetupProviderModelCombos(comboProvider, comboModel);

  form->addRow(tr("ID:"), editId);
  form->addRow(tr("Label:"), editLabel);
  form->addRow(tr("Prompt:"), editPrompt);
  form->addRow(tr("Provider:"), comboProvider);
  form->addRow(tr("Model:"), comboModel);
  form->addRow(tr("Context Lines:"), spinContextLines);
  form->addRow(tr("Candidate Count:"), spinCandidates);
  form->addRow(tr("Timeout (ms):"), spinTimeout);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  vinput::scene::Definition def;
  def.id = editId->text().trimmed().toStdString();
  def.label = editLabel->text().trimmed().toStdString();
  def.prompt = editPrompt->toPlainText().toStdString();
  def.provider_id = comboProvider->currentText().toStdString();
  def.model = comboModel->currentText().trimmed().toStdString();
  def.context_lines = spinContextLines->value();
  def.candidate_count = spinCandidates->value();
  def.timeout_ms = spinTimeout->value();

  CoreConfig config = ConfigManager::Get().Load();
  std::string err;
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  if (!vinput::scene::AddScene(&sc, def, &err)) {
      QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
      return;
  }
  FromSceneConfig(config.scenes, sc);
  if (!ConfigManager::Get().Save(config)) {
      QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
      return;
  }
  refreshSceneList();
  emit configChanged();
}

void LlmPage::onSceneEdit() {
  auto *item = listScenes_->currentItem();
  if (!item)
    return;

  QString scene_id = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  const vinput::scene::Definition *found = vinput::scene::Find(sc, scene_id.toStdString());
  if (!found) return;

  QDialog dialog(this);
  dialog.setWindowTitle(tr("Edit Scene"));

  auto *form = new QFormLayout();
  auto *editId = new QLineEdit(scene_id);
  editId->setReadOnly(true);
  auto *editLabel = new QLineEdit(QString::fromStdString(found->label));
  auto *editPrompt = new QTextEdit();
  editPrompt->setPlainText(QString::fromStdString(found->prompt));
  editPrompt->setMaximumHeight(100);
  auto *comboProvider = new QComboBox();
  auto *comboModel = new QComboBox();
  auto *spinTimeout = new QSpinBox();
  spinTimeout->setRange(1000, 300000);
  spinTimeout->setSingleStep(1000);
  spinTimeout->setValue(found->timeout_ms);
  spinTimeout->setSuffix(" ms");
  auto *spinCandidates = new QSpinBox();
  spinCandidates->setRange(kMinCandidateCount, kMaxCandidateCount);
  spinCandidates->setValue(found->candidate_count);
  auto *spinContextLines = new QSpinBox();
  spinContextLines->setRange(0, 9999);
  spinContextLines->setValue(found->context_lines);

  SetupProviderModelCombos(comboProvider, comboModel,
                           QString::fromStdString(found->provider_id),
                           QString::fromStdString(found->model));

  form->addRow(tr("ID:"), editId);
  form->addRow(tr("Label:"), editLabel);
  form->addRow(tr("Prompt:"), editPrompt);
  form->addRow(tr("Provider:"), comboProvider);
  form->addRow(tr("Model:"), comboModel);
  form->addRow(tr("Context Lines:"), spinContextLines);
  form->addRow(tr("Candidate Count:"), spinCandidates);
  form->addRow(tr("Timeout (ms):"), spinTimeout);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto *dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  vinput::scene::Definition def = *found;
  def.label = editLabel->text().trimmed().toStdString();
  def.prompt = editPrompt->toPlainText().toStdString();
  def.provider_id = comboProvider->currentText().toStdString();
  def.model = comboModel->currentText().trimmed().toStdString();
  def.context_lines = spinContextLines->value();
  def.candidate_count = spinCandidates->value();
  def.timeout_ms = spinTimeout->value();

  std::string err;
  sc = ToSceneConfig(config.scenes);
  if (!vinput::scene::UpdateScene(&sc, scene_id.toStdString(), def, &err)) {
      QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
      return;
  }
  FromSceneConfig(config.scenes, sc);
  if (!ConfigManager::Get().Save(config)) {
      QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
      return;
  }
  refreshSceneList();
  emit configChanged();
}

void LlmPage::onSceneRemove() {
  auto *item = listScenes_->currentItem();
  if (!item)
    return;

  QString scene_id = item->data(Qt::UserRole).toString();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove scene '%1'?").arg(scene_id));
  if (response != QMessageBox::Yes)
    return;

  CoreConfig config = ConfigManager::Get().Load();
  std::string err;
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  if (!vinput::scene::RemoveScene(&sc, scene_id.toStdString(), true, &err)) {
      QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
      return;
  }
  FromSceneConfig(config.scenes, sc);
  if (!ConfigManager::Get().Save(config)) {
      QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
      return;
  }
  refreshSceneList();
  emit configChanged();
}

void LlmPage::onSceneSetActive() {
  auto *item = listScenes_->currentItem();
  if (!item)
    return;

  QString scene_id = item->data(Qt::UserRole).toString();
  
  CoreConfig config = vinput::gui::ConfigManager::Get().Load();
  std::string err;
  
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  if (!vinput::scene::SetActiveScene(&sc, scene_id.toStdString(), &err)) {
      QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
      return;
  }
  FromSceneConfig(config.scenes, sc);
  
  if (!vinput::gui::ConfigManager::Get().Save(config)) {
      QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
      return;
  }

  refreshSceneList();
  emit configChanged();
}

}  // namespace vinput::gui
