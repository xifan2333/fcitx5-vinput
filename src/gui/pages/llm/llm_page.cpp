#include "pages/llm/llm_page.h"

#include <QCheckBox>
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
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QtCore/qbytearray.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qobject.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qpointer.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qtimer.h>
#include <QtCore/qurl.h>
#include <QtGui/qfontmetrics.h>
#include <QtNetwork/qnetworkaccessmanager.h>
#include <QtNetwork/qnetworkreply.h>
#include <QtNetwork/qnetworkrequest.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qdialogbuttonbox.h>
#include <QtWidgets/qformlayout.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qplaintextedit.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qspinbox.h>
#include <QtWidgets/qtextedit.h>
#include <QtWidgets/qwidget.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <glaze/core/common.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/json/json_access.h"
#include "common/llm/adapter_manager.h"
#include "common/llm/defaults.h"
#include "common/llm/provider_model_cache.h"
#include "common/registry/registry_i18n.h"
#include "common/registry/registry_scripts.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/interpolation.h"

#include "cli/runtime/dbus_client.h"

#include "gui/utils/config_manager.h"
#include "gui/utils/i18n_cache.h"

#include "dialogs/adapter_dialog.h"
#include "utils/gui_helpers.h"

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
bool ParseExtraBodyInput(QWidget* parent, const QString& text, glz::json_t* out) {
  const std::string trimmed = text.trimmed().toStdString();
  *out = glz::json_t(glz::json_t::object_t{});
  if (trimmed.empty()) {
    return true;
  }
  glz::json_t parsed;
  if (!vinput::json::IsSingleJsonValue(trimmed)) {
    QMessageBox::warning(
        parent, LlmPage::tr("Error"),
        LlmPage::tr("Extra body must be a JSON object (e.g. {\"enable_thinking\": false})."));
    return false;
  }
  if (const auto ec = glz::read_json(parsed, trimmed); bool(ec)) {
    QMessageBox::warning(parent, LlmPage::tr("Error"),
                         LlmPage::tr("Invalid extra_body JSON: %1")
                             .arg(QString::fromStdString(glz::format_error(ec, trimmed))));
    return false;
  }
  if (!parsed.is_object()) {
    QMessageBox::warning(
        parent, LlmPage::tr("Error"),
        LlmPage::tr("Extra body must be a JSON object (e.g. {\"enable_thinking\": false})."));
    return false;
  }
  *out = std::move(parsed);
  return true;
}

QPlainTextEdit* MakeExtraBodyEditor(const glz::json_t* prefill = nullptr) {
  auto* edit = new QPlainTextEdit();
  edit->setPlaceholderText(LlmPage::tr("Optional JSON object merged into each request body, e.g. "
                                       "{\"enable_thinking\": false}"));
  edit->setTabChangesFocus(true);
  const QFontMetrics fm(edit->font());
  edit->setFixedHeight((fm.lineSpacing() * 5) + 8);
  if (prefill != nullptr && prefill->is_object() && !prefill->empty()) {
    edit->setPlainText(QString::fromStdString(vinput::json::ToPrettyText(*prefill)));
  }
  return edit;
}

struct ProviderFormResult {
  std::string id;
  std::string base_url;
  std::string api_key;
  glz::json_t extra_body;
};

std::optional<ProviderFormResult> ShowProviderDialog(QWidget* parent, const QString& title,
                                                     const LlmProvider* initial = nullptr) {
  auto* form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  auto* editName = new QLineEdit();
  if (initial != nullptr) {
    editName->setText(QString::fromStdString(initial->id));
    editName->setReadOnly(true);
  }

  auto* editBaseUrl =
      new QLineEdit(initial != nullptr ? QString::fromStdString(initial->base_url) : QString{});
  auto* editApiKey = new QLineEdit();
  editApiKey->setEchoMode(QLineEdit::Password);
  if (initial != nullptr) {
    editApiKey->setPlaceholderText(LlmPage::tr("Leave empty to keep current key"));
  }
  auto* editExtraBody = MakeExtraBodyEditor(initial != nullptr ? &initial->extra_body : nullptr);

  form->addRow(LlmPage::tr("Name:"), editName);
  form->addRow(LlmPage::tr("Base URL:"), editBaseUrl);
  form->addRow(LlmPage::tr("API Key:"), editApiKey);
  form->addRow(LlmPage::tr("Extra body (JSON):"), editExtraBody);

  if (!RunFormDialog(parent, title, form)) {
    return std::nullopt;
  }

  const QString name_text = editName->text().trimmed();
  const QString base_url_text = editBaseUrl->text().trimmed();
  QString validation_error;
  if (!ValidateProviderInput(name_text, base_url_text, &validation_error)) {
    QMessageBox::warning(parent, LlmPage::tr("Error"), validation_error);
    return std::nullopt;
  }

  glz::json_t extra_body;
  if (!ParseExtraBodyInput(parent, editExtraBody->toPlainText(), &extra_body)) {
    return std::nullopt;
  }

  return ProviderFormResult{
      .id = name_text.toStdString(),
      .base_url = base_url_text.toStdString(),
      .api_key = editApiKey->text().trimmed().toStdString(),
      .extra_body = std::move(extra_body),
  };
}

constexpr int kDefaultTimeoutMs = vinput::scene::kDefaultTimeoutMs;
constexpr int kDefaultUiLlmMaxCandidates = 3;

QString SceneLabelForGui(const vinput::scene::Definition& scene) {
  if (scene.id == vinput::scene::kRawSceneId || scene.label == vinput::scene::kRawSceneLabelKey) {
    return LlmPage::tr("Raw");
  }
  if (scene.id == vinput::scene::kCommandSceneId ||
      scene.label == vinput::scene::kCommandSceneLabelKey) {
    return LlmPage::tr("Command");
  }
  if (!scene.label.empty()) {
    return QString::fromStdString(scene.label);
  }
  return QString::fromStdString(scene.id);
}

// Adapters have no config-side label/title field; registry i18n is the only
// user-facing name. Fall back to id when the cache has no entry yet.
QString AdapterTitleForGui(const std::string& adapter_id) {
  const auto i18n_map = I18nCache::Get().GetMap();
  return QString::fromStdString(
      vinput::registry::LookupI18n(i18n_map, adapter_id + ".title", adapter_id));
}

// Convert between AdapterData (GUI dialog) and LlmAdapter (config).
AdapterData AdapterDataFromConfig(const LlmAdapter& a) {
  return {.id = a.id, .command = a.command, .args = a.args, .env = a.env, .autoStart = a.autoStart};
}

LlmAdapter LlmAdapterFromDialog(const AdapterData& d) {
  return {.id = d.id, .command = d.command, .args = d.args, .env = d.env, .autoStart = d.autoStart};
}

std::optional<vinput::scene::Definition>
ShowSceneDialog(QWidget* parent, const QString& title,
                const vinput::scene::Definition* initial = nullptr) {
  auto* form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  auto* editId = new QLineEdit();
  if (initial != nullptr) {
    editId->setText(QString::fromStdString(initial->id));
    editId->setReadOnly(true);
  }
  auto* editLabel =
      new QLineEdit(initial != nullptr ? QString::fromStdString(initial->label) : QString{});
  auto* editPrompt = new QTextEdit();
  if (initial != nullptr) {
    editPrompt->setPlainText(QString::fromStdString(initial->prompt));
  }
  editPrompt->setMaximumHeight(100);

  auto* comboProvider = new QComboBox();
  auto* comboModel = new QComboBox();
  auto* spinTimeout = new QSpinBox();
  spinTimeout->setRange(1000, 300000);
  spinTimeout->setSingleStep(1000);
  spinTimeout->setValue(initial != nullptr ? initial->timeout_ms : kDefaultTimeoutMs);
  spinTimeout->setSuffix(" ms");

  auto* spinLlmMaxCandidates = new QSpinBox();
  spinLlmMaxCandidates->setRange(vinput::scene::kMinLlmMaxCandidates,
                                 vinput::scene::kMaxLlmMaxCandidates);
  spinLlmMaxCandidates->setValue(initial != nullptr ? initial->llm_max_candidates
                                                    : kDefaultUiLlmMaxCandidates);
  spinLlmMaxCandidates->setToolTip(
      LlmPage::tr("Maximum number of LLM rewrite results (0 to disable LLM)."));

  auto* spinContextLines = new QSpinBox();
  spinContextLines->setRange(0, 9999);
  spinContextLines->setValue(initial != nullptr ? initial->context_lines
                                                : vinput::scene::kDefaultContextLines);

  auto* chkRawCand = new QCheckBox(LlmPage::tr("Include raw transcript in candidate options"));
  chkRawCand->setChecked(initial != nullptr ? initial->raw_cand : true);
  chkRawCand->setToolTip(
      LlmPage::tr("Whether to include raw speech text as option 1 in candidate options."));

  auto* chkRawPrev = new QCheckBox(LlmPage::tr("Preview raw transcript during postprocessing"));
  chkRawPrev->setChecked(initial != nullptr ? initial->raw_prev : true);
  chkRawPrev->setToolTip(LlmPage::tr(
      "Whether to preview raw speech text in the floating panel during postprocessing."));

  if (initial != nullptr) {
    SetupProviderModelCombos(comboProvider, comboModel,
                             QString::fromStdString(initial->provider_id),
                             QString::fromStdString(initial->model));
  } else {
    SetupProviderModelCombos(comboProvider, comboModel);
  }

  form->addRow(LlmPage::tr("ID:"), editId);
  form->addRow(LlmPage::tr("Label:"), editLabel);
  form->addRow(LlmPage::tr("Prompt:"), editPrompt);
  form->addRow(LlmPage::tr("Provider:"), comboProvider);
  form->addRow(LlmPage::tr("Model:"), comboModel);
  form->addRow(LlmPage::tr("Context Lines:"), spinContextLines);
  form->addRow(LlmPage::tr("LLM Max Results:"), spinLlmMaxCandidates);
  form->addRow(LlmPage::tr("Timeout (ms):"), spinTimeout);
  form->addRow(QString(), chkRawCand);
  form->addRow(QString(), chkRawPrev);

  if (!RunFormDialog(parent, title, form)) {
    return std::nullopt;
  }

  vinput::scene::Definition def = initial != nullptr ? *initial : vinput::scene::Definition{};
  def.id = editId->text().trimmed().toStdString();
  def.label = editLabel->text().trimmed().toStdString();
  def.prompt = editPrompt->toPlainText().trimmed().toStdString();
  def.provider_id = comboProvider->currentText().trimmed().toStdString();
  def.model = comboModel->currentText().trimmed().toStdString();
  def.context_lines = spinContextLines->value();
  def.llm_max_candidates = spinLlmMaxCandidates->value();
  def.timeout_ms = spinTimeout->value();
  def.raw_cand = chkRawCand->isChecked();
  def.raw_prev = chkRawPrev->isChecked();

  return def;
}

} // namespace

LlmPage::LlmPage(QWidget* parent)
    : QWidget(parent), listProviders_(new QListWidget()), listAdapters_(new QListWidget()),
      listScenes_(new QListWidget()) {
  auto* layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(tr("<b>Providers</b>")));
  auto* listLayout = new QHBoxLayout();
  listLayout->addWidget(listProviders_);

  auto* btnLayout = new QVBoxLayout();
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

  auto* adapterLayout = new QHBoxLayout();
  adapterLayout->addWidget(listAdapters_);

  auto* adapterBtnLayout = new QVBoxLayout();
  btnAdapterEdit_ = new QPushButton(tr("Edit"));
  btnAdapterRemove_ = new QPushButton(tr("Remove"));
  btnAdapterStart_ = new QPushButton(tr("Start"));
  btnAdapterStop_ = new QPushButton(tr("Stop"));
  btnAdapterRefresh_ = new QPushButton(tr("Refresh"));
  adapterBtnLayout->addWidget(btnAdapterEdit_);
  adapterBtnLayout->addWidget(btnAdapterRemove_);
  adapterBtnLayout->addWidget(btnAdapterStart_);
  adapterBtnLayout->addWidget(btnAdapterStop_);
  adapterBtnLayout->addWidget(btnAdapterRefresh_);
  adapterBtnLayout->addStretch();
  adapterLayout->addLayout(adapterBtnLayout);

  layout->addWidget(new QLabel(tr("<b>LLM Adapters</b>")));
  layout->addLayout(adapterLayout);

  // Scenes section
  layout->addWidget(new QLabel(tr("<b>Scenes</b>")));
  auto* sceneLayout = new QHBoxLayout();
  sceneLayout->addWidget(listScenes_);

  auto* sceneBtnLayout = new QVBoxLayout();
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
  connect(btnAdapterEdit_, &QPushButton::clicked, this, &LlmPage::onAdapterEdit);
  connect(btnAdapterRemove_, &QPushButton::clicked, this, &LlmPage::onAdapterRemove);
  connect(btnAdapterStart_, &QPushButton::clicked, this, &LlmPage::onAdapterStart);
  connect(btnAdapterStop_, &QPushButton::clicked, this, &LlmPage::onAdapterStop);
  connect(btnAdapterRefresh_, &QPushButton::clicked, this, &LlmPage::refreshAdapterList);
  connect(listAdapters_, &QListWidget::itemSelectionChanged, this, &LlmPage::updateAdapterButtons);
  connect(btnSceneAdd_, &QPushButton::clicked, this, &LlmPage::onSceneAdd);
  connect(btnSceneEdit_, &QPushButton::clicked, this, &LlmPage::onSceneEdit);
  connect(btnSceneRemove_, &QPushButton::clicked, this, &LlmPage::onSceneRemove);
  connect(btnSceneSetActive_, &QPushButton::clicked, this, &LlmPage::onSceneSetActive);
  connect(&I18nCache::Get(), &I18nCache::mapUpdated, this, &LlmPage::refreshAdapterList);

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
  const CoreConfig config = ConfigManager::Get().Load();

  for (const auto& provider : config.llm.providers) {
    const QString name = QString::fromStdString(provider.id);
    const QString base_url = QString::fromStdString(provider.base_url);
    const QString display = QString("%1 @ %2").arg(name, base_url);

    auto* item = new QListWidgetItem(display, listProviders_);
    item->setData(Qt::UserRole, name);
  }
}

void LlmPage::refreshAdapterList() {
  listAdapters_->clear();
  const CoreConfig config = ConfigManager::Get().Load();

  for (const auto& adapter : config.llm.adapters) {
    const QString id = QString::fromStdString(adapter.id);
    const QString title = AdapterTitleForGui(adapter.id);
    const bool running = vinput::adapter::IsRunning(adapter.id);

    QString display = title + " · " + (running ? tr("running") : tr("stopped"));
    display += " · " + (adapter.autoStart ? tr("autostart") : tr("manual"));
    const QString command = QString::fromStdString(adapter.command);
    if (!command.isEmpty()) {
      display += " · " + command;
    }

    auto* item = new QListWidgetItem(display, listAdapters_);
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, running);
    item->setData(Qt::UserRole + 2, title);
    item->setData(Qt::UserRole + 3, adapter.autoStart);

    // Build tooltip
    QString tooltip;
    tooltip += tr("Autostart: %1").arg(adapter.autoStart ? tr("enabled") : tr("disabled"));
    if (!command.isEmpty()) {
      tooltip += "\n" + tr("Command: %1").arg(command);
    }
    if (!adapter.args.empty()) {
      std::string joined_args;
      for (const auto& a : adapter.args) {
        if (!joined_args.empty()) {
          joined_args += ' ';
        }
        joined_args += a;
      }
      tooltip += "\n" + tr("Args: %1").arg(QString::fromStdString(joined_args));
    }
    if (!adapter.env.empty()) {
      std::string joined_env;
      for (const auto& [k, v] : adapter.env) {
        if (!joined_env.empty()) {
          joined_env += ' ';
        }
        joined_env += k;
        joined_env += '=';
        joined_env += v;
      }
      tooltip += "\n" + tr("Env: %1").arg(QString::fromStdString(joined_env));
    }
    item->setToolTip(tooltip.trimmed());
  }
  updateAdapterButtons();
}

void LlmPage::onLlmAdd() {
  const auto result = ShowProviderDialog(this, tr("Add LLM Provider"));
  if (!result.has_value()) {
    return;
  }

  CoreConfig config = ConfigManager::Get().Load();
  if (ResolveLlmProvider(config, result->id) != nullptr) {
    QMessageBox::warning(
        this, tr("Error"),
        tr("LLM provider '%1' already exists.").arg(QString::fromStdString(result->id)));
    return;
  }

  LlmProvider provider;
  provider.id = result->id;
  provider.base_url = result->base_url;
  provider.api_key = result->api_key;
  provider.extra_body = std::move(result->extra_body);
  config.llm.providers.push_back(std::move(provider));

  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshLlmList();
  configChanged();
}

void LlmPage::onLlmEdit() {
  auto* item = listProviders_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString provider_name = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  const LlmProvider* prov = ResolveLlmProvider(config, provider_name.toStdString());
  if (prov == nullptr) {
    return;
  }

  const auto result = ShowProviderDialog(this, tr("Edit LLM Provider"), prov);
  if (!result.has_value()) {
    return;
  }

  auto& providers = config.llm.providers;
  auto it = std::ranges::find_if(
      providers, [&](const LlmProvider& p) { return p.id == provider_name.toStdString(); });
  if (it != providers.end()) {
    it->base_url = result->base_url;
    if (!result->api_key.empty()) {
      it->api_key = result->api_key;
    }
    it->extra_body = std::move(result->extra_body);
    if (!ConfigManager::Get().Save(config)) {
      QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
      return;
    }
  }

  refreshLlmList();
  configChanged();
}

void LlmPage::onLlmRemove() {
  auto* item = listProviders_->currentItem();
  if (item == nullptr) {
    return;
  }

  QString provider_name = item->data(Qt::UserRole).toString();
  auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove LLM provider '%1'?").arg(provider_name));
  if (response != QMessageBox::Yes) {
    return;
  }

  CoreConfig config = ConfigManager::Get().Load();
  auto& providers = config.llm.providers;
  auto it = std::ranges::find_if(
      providers, [&](const LlmProvider& p) { return p.id == provider_name.toStdString(); });
  int cleared_scene_refs = 0;
  if (it != providers.end()) {
    providers.erase(it);
    vinput::llm::RemoveProviderModels(provider_name.toStdString());
    vinput::scene::Config sc = ToSceneConfig(config.scenes);
    cleared_scene_refs = vinput::scene::ClearProviderReferences(&sc, provider_name.toStdString());
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
  configChanged();
  if (cleared_scene_refs > 0) {
    QMessageBox::information(this, tr("Scenes Updated"),
                             tr("Removed provider '%1' and cleared it from %2 scene(s).")
                                 .arg(provider_name)
                                 .arg(cleared_scene_refs));
  }
}

void LlmPage::onLlmTest() {
  auto* item = listProviders_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString provider_id = item->data(Qt::UserRole).toString();
  const CoreConfig config = ConfigManager::Get().Load();
  const LlmProvider* provider = ResolveLlmProvider(config, provider_id.toStdString());
  if (provider == nullptr) {
    QMessageBox::warning(this, tr("Error"), tr("Provider '%1' not found.").arg(provider_id));
    return;
  }

  QString url = QString::fromStdString(provider->base_url);
  if (url.isEmpty()) {
    QMessageBox::warning(this, tr("Error"), tr("Provider base_url is empty."));
    return;
  }
  while (!url.isEmpty() && url.endsWith(u'/')) {
    url.chop(1);
  }
  url += vinput::llm::kOpenAiModelsPath;

  btnLlmTest_->setEnabled(false);
  btnLlmTest_->setText(tr("Testing..."));

  auto* nam = new QNetworkAccessManager(this);
  QNetworkRequest req{QUrl(url)};
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  const std::string resolved_key = vinput::interpolate::InterpolateEnv(provider->api_key);
  if (!resolved_key.empty()) {
    const QByteArray bearer =
        QByteArray(vinput::llm::kBearerPrefix) + QByteArray::fromStdString(resolved_key);
    req.setRawHeader(vinput::llm::kAuthorizationHeader, bearer);
  }

  QNetworkReply* reply = nam->get(req);
  auto* timeout = new QTimer(reply);
  timeout->setSingleShot(true);
  QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
    if (!reply->isFinished()) {
      reply->abort();
    }
  });
  timeout->start(10000);

  QObject::connect(
      reply, &QNetworkReply::finished, this, [this, reply, timeout, nam, provider_id]() {
        timeout->stop();
        btnLlmTest_->setEnabled(true);
        btnLlmTest_->setText(tr("Test"));

        if (reply->error() != QNetworkReply::NoError) {
          QMessageBox::warning(
              this, tr("Test Failed"),
              tr("Connection to '%1' failed:\n%2").arg(provider_id, reply->errorString()));
        } else {
          QStringList models;
          if (!ParseOpenAiModelsResponse(reply->readAll(), &models)) {
            QMessageBox::warning(this, tr("Test Failed"),
                                 tr("Invalid response from '%1'.").arg(provider_id));
          } else {
            std::vector<std::string> models_vec;
            models_vec.reserve(models.size());
            for (const auto& m : models) {
              models_vec.push_back(m.toStdString());
            }
            vinput::llm::SaveProviderModels(provider_id.toStdString(), models_vec);
            QString msg =
                tr("Connected to '%1'.\nFound %2 model(s).").arg(provider_id).arg(models.size());
            if (!models.empty()) {
              msg += "\n\n" + models.join('\n');
            }
            QMessageBox::information(this, tr("Test Succeeded"), msg);
          }
        }
        reply->deleteLater();
        nam->deleteLater();
      });
}

void LlmPage::onAdapterEdit() {
  auto* item = listAdapters_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString adapter_id = item->data(Qt::UserRole).toString();

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
    const QString adapter_title = AdapterTitleForGui(adapter_id.toStdString());
    QMessageBox::warning(this, tr("Error"),
                         tr("Adapter '%1' not found in configuration.").arg(adapter_title));
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
  configChanged();
}

void LlmPage::onAdapterRemove() {
  auto* item = listAdapters_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString adapter_id = item->data(Qt::UserRole).toString();
  QString adapter_title = item->data(Qt::UserRole + 2).toString();
  if (adapter_title.isEmpty()) {
    adapter_title = AdapterTitleForGui(adapter_id.toStdString());
  }

  const auto response = QMessageBox::question(
      this, tr("Confirm"),
      tr("Are you sure you want to remove LLM adapter '%1'?").arg(adapter_title));
  if (response != QMessageBox::Yes) {
    return;
  }

  if (vinput::adapter::IsRunning(adapter_id.toStdString())) {
    vinput::cli::DbusClient dbus;
    std::string err;
    dbus.StopAdapter(adapter_id.toStdString(), &err);
  }

  CoreConfig config = ConfigManager::Get().Load();
  const auto removed = std::ranges::remove_if(
      config.llm.adapters, [&](const LlmAdapter& a) { return a.id == adapter_id.toStdString(); });
  if (removed.empty()) {
    return;
  }
  config.llm.adapters.erase(removed.begin(), removed.end());

  const auto managed_path = vinput::script::DefaultLocalScriptPath(
      vinput::script::Kind::kLlmAdapter, adapter_id.toStdString());
  if (!managed_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(managed_path, ec);
  }

  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::critical(this, tr("Error"), tr("Failed to save config."));
    return;
  }

  refreshAdapterList();
  configChanged();
}

void LlmPage::updateAdapterButtons() {
  auto* item = listAdapters_->currentItem();
  if (item == nullptr) {
    btnAdapterEdit_->setEnabled(false);
    btnAdapterRemove_->setEnabled(false);
    btnAdapterStart_->setEnabled(false);
    btnAdapterStop_->setEnabled(false);
    return;
  }
  const bool running = item->data(Qt::UserRole + 1).toBool();
  btnAdapterEdit_->setEnabled(true);
  btnAdapterRemove_->setEnabled(true);
  btnAdapterStart_->setEnabled(!running);
  btnAdapterStop_->setEnabled(running);
}

void LlmPage::executeAdapterControl(bool start) {
  auto* item = listAdapters_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString adapter_id = item->data(Qt::UserRole).toString();
  QString adapter_title = item->data(Qt::UserRole + 2).toString();
  if (adapter_title.isEmpty()) {
    adapter_title = AdapterTitleForGui(adapter_id.toStdString());
  }

  RunAdapterControlGuarded(
      this, {btnAdapterStart_, btnAdapterStop_}, adapter_id.toStdString(), start,
      [this, start, adapter_title]() {
        if (start) {
          QMessageBox::information(this, tr("LLM Adapter Started"),
                                   tr("Adapter '%1' started.").arg(adapter_title));
        }
        refreshAdapterList();
      },
      tr("Error"), [this]() { updateAdapterButtons(); });
}

void LlmPage::onAdapterStart() {
  executeAdapterControl(true);
}

void LlmPage::onAdapterStop() {
  executeAdapterControl(false);
}

void LlmPage::refreshSceneList() {
  listScenes_->clear();
  const CoreConfig config = ConfigManager::Get().Load();

  for (const auto& scene : config.scenes.definitions) {
    QString label = SceneLabelForGui(scene);
    const bool active = (scene.id == config.scenes.activeScene);
    if (active) {
      label += " *";
    }
    auto* item = new QListWidgetItem(label, listScenes_);
    item->setData(Qt::UserRole, QString::fromStdString(scene.id));
  }
}

void LlmPage::onSceneAdd() {
  const auto def = ShowSceneDialog(this, tr("Add Scene"));
  if (!def.has_value()) {
    return;
  }

  CoreConfig config = ConfigManager::Get().Load();
  std::string err;
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  if (!vinput::scene::AddScene(&sc, *def, &err)) {
    QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  FromSceneConfig(config.scenes, sc);
  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshSceneList();
  configChanged();
}

void LlmPage::onSceneEdit() {
  auto* item = listScenes_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString scene_id = item->data(Qt::UserRole).toString();

  CoreConfig config = ConfigManager::Get().Load();
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  const vinput::scene::Definition* found = vinput::scene::Find(sc, scene_id.toStdString());
  if (found == nullptr) {
    return;
  }

  const auto def = ShowSceneDialog(this, tr("Edit Scene"), found);
  if (!def.has_value()) {
    return;
  }

  std::string err;
  sc = ToSceneConfig(config.scenes);
  if (!vinput::scene::UpdateScene(&sc, scene_id.toStdString(), *def, &err)) {
    QMessageBox::warning(this, tr("Error"), QString::fromStdString(err));
    return;
  }
  FromSceneConfig(config.scenes, sc);
  if (!ConfigManager::Get().Save(config)) {
    QMessageBox::warning(this, tr("Error"), tr("Failed to save config."));
    return;
  }
  refreshSceneList();
  configChanged();
}

void LlmPage::onSceneRemove() {
  auto* item = listScenes_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString scene_id = item->data(Qt::UserRole).toString();
  CoreConfig config = ConfigManager::Get().Load();
  vinput::scene::Config sc = ToSceneConfig(config.scenes);
  QString scene_label = scene_id;
  if (const auto* scene = vinput::scene::Find(sc, scene_id.toStdString())) {
    scene_label = SceneLabelForGui(*scene);
  }
  auto response = QMessageBox::question(
      this, tr("Confirm"), tr("Are you sure you want to remove scene '%1'?").arg(scene_label));
  if (response != QMessageBox::Yes) {
    return;
  }

  std::string err;
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
  configChanged();
}

void LlmPage::onSceneSetActive() {
  auto* item = listScenes_->currentItem();
  if (item == nullptr) {
    return;
  }

  const QString scene_id = item->data(Qt::UserRole).toString();

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
  configChanged();
}

} // namespace vinput::gui
