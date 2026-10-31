#include "utils/gui_helpers.h"

#include <QtCore/qbytearray.h>
#include <QtCore/qcontainerfwd.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qhash.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#if __has_include(<QtCore/qjsonparseerror.h>)
#include <QtCore/qjsonparseerror.h>
#endif
#include <QtCore/qjsonobject.h>
#include <QtCore/qlist.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qobject.h>
#include <QtCore/qpointer.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qthreadpool.h>
#include <QtCore/qtimer.h>
#include <QtCore/qurl.h>
#include <QtNetwork/qnetworkaccessmanager.h>
#include <QtNetwork/qnetworkreply.h>
#include <QtNetwork/qnetworkrequest.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qdialogbuttonbox.h>
#include <QtWidgets/qformlayout.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qtablewidget.h>
#include <QtWidgets/qwidget.h>
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/llm/defaults.h"
#include "common/llm/provider_model_cache.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/interpolation.h"
#include "common/utils/url_utils.h"

#include "cli/runtime/dbus_client.h"

#include "gui/utils/config_manager.h"

namespace vinput::gui {

void RunAdapterControlAsync(QWidget* context, std::string adapter_id, bool start,
                            std::function<void(bool ok, const std::string& err)> callback) {
  const QPointer<QWidget> self(context);
  QThreadPool::globalInstance()->start(
      [self, adapter_id = std::move(adapter_id), start, callback = std::move(callback)]() mutable {
        vinput::cli::DbusClient dbus;
        std::string err;
        const bool ok =
            start ? dbus.StartAdapter(adapter_id, &err) : dbus.StopAdapter(adapter_id, &err);
        QMetaObject::invokeMethod(self, [self, callback = std::move(callback), ok, err]() mutable {
          if (!self) {
            return;
          }
          callback(ok, err);
        });
      });
}

namespace {
struct GuiHelpers {
  Q_DECLARE_TR_FUNCTIONS(GuiHelpers)
};
} // namespace

bool RunFormDialog(QWidget* parent, const QString& title, QFormLayout* form,
                   const std::function<void()>& on_accepted) {
  if (form == nullptr) {
    return false;
  }
  QDialog dialog(parent);
  dialog.setWindowTitle(title);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  auto* dlgLayout = new QVBoxLayout(&dialog);
  dlgLayout->addLayout(form);
  dlgLayout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted) {
    return false;
  }
  if (on_accepted) {
    on_accepted();
  }
  return true;
}

bool ParseOpenAiModelsResponse(const QByteArray& payload, QStringList* models_out,
                               QString* error_out) {
  if (models_out == nullptr) {
    return false;
  }
  models_out->clear();

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject() ||
      !doc.object().value("data").isArray()) {
    if (error_out != nullptr) {
      *error_out = GuiHelpers::tr("Provider returned invalid JSON for /v1/models.");
    }
    return false;
  }

  const QJsonArray data = doc.object().value("data").toArray();
  for (const auto& v : data) {
    const QString id = v.toObject().value("id").toString();
    if (!id.isEmpty()) {
      models_out->append(id);
    }
  }
  models_out->removeDuplicates();
  models_out->sort();
  return true;
}

QNetworkReply* StartOpenAiModelsRequest(QNetworkAccessManager* nam, const std::string& base_url,
                                        const std::string& api_key, int timeout_ms) {
  if (nam == nullptr) {
    return nullptr;
  }
  const std::string url_str = vinput::url::JoinPath(base_url, vinput::llm::kOpenAiModelsPath);
  QNetworkRequest req{QUrl(QString::fromStdString(url_str))};
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  const std::string resolved_key = vinput::interpolate::InterpolateEnv(api_key);
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
  QObject::connect(reply, &QNetworkReply::finished, timeout, &QTimer::stop);
  timeout->start(timeout_ms);
  return reply;
}

bool SaveProviderModels(const std::string& provider_id, const QStringList& models) {
  std::vector<std::string> models_vec;
  models_vec.reserve(models.size());
  for (const auto& m : models) {
    models_vec.push_back(m.toStdString());
  }
  return vinput::llm::SaveProviderModels(provider_id, models_vec);
}

bool ProcessAndSaveProviderModels(const QByteArray& payload, const std::string& provider_id,
                                  QString* joined_models_out, int* model_count_out,
                                  QString* error_out) {
  QStringList models;
  if (!ParseOpenAiModelsResponse(payload, &models, error_out)) {
    return false;
  }
  SaveProviderModels(provider_id, models);
  if (model_count_out != nullptr) {
    *model_count_out = static_cast<int>(models.size());
  }
  if (joined_models_out != nullptr) {
    *joined_models_out = models.join('\n');
  }
  return true;
}

QStringList NonEmptyLines(const QString& text) {
  QStringList lines;
  for (const QString& line : text.split('\n')) {
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty()) {
      lines.push_back(trimmed);
    }
  }
  return lines;
}

QTableWidgetItem* MakeCell(const QString& text, const QString& data) {
  auto* cell = new QTableWidgetItem(text);
  cell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
  if (!data.isEmpty()) {
    cell->setData(Qt::UserRole, data);
  }
  return cell;
}

QString FormatScriptUpdateStatus(const vinput::script::ResourceVersionInfo& ver_info,
                                 bool installed) {
  if (ver_info.status == vinput::script::UpdateStatus::kIncompatible) {
    return GuiHelpers::tr("incompatible (requires vinput %1)")
        .arg(QString::fromStdString(ver_info.vinput_version));
  }
  if (installed) {
    if (ver_info.status == vinput::script::UpdateStatus::kUpdateAvailable) {
      if (!ver_info.installed_version.empty() && !ver_info.remote_version.empty()) {
        return GuiHelpers::tr("update available (%1 -> %2)")
            .arg(QString::fromStdString(ver_info.installed_version),
                 QString::fromStdString(ver_info.remote_version));
      }
      return GuiHelpers::tr("update available");
    }
    if (!ver_info.installed_version.empty()) {
      return GuiHelpers::tr("installed (%1)")
          .arg(QString::fromStdString(ver_info.installed_version));
    }
    return GuiHelpers::tr("installed");
  }
  return GuiHelpers::tr("available");
}

bool ValidateProviderInput(const QString& name, const QString& base_url, QString* error_out) {
  if (name.trimmed().isEmpty()) {
    if (error_out != nullptr) {
      *error_out = GuiHelpers::tr("Provider name must not be empty.");
    }
    return false;
  }

  const QUrl url = QUrl::fromUserInput(base_url.trimmed());
  if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty() ||
      (url.scheme() != "http" && url.scheme() != "https")) {
    if (error_out != nullptr) {
      *error_out = GuiHelpers::tr("Base URL must be a valid http:// or https:// URL.");
    }
    return false;
  }

  return true;
}

bool ParseCommandEnv(const QString& text, std::map<std::string, std::string>* env,
                     QString* error_out) {
  if (env == nullptr) {
    return false;
  }

  env->clear();
  const QStringList lines = NonEmptyLines(text);
  return std::ranges::all_of(lines, [&](const QString& line) {
    const auto pos = line.indexOf('=');
    if (pos <= 0) {
      if (error_out != nullptr) {
        *error_out = GuiHelpers::tr("Invalid env entry '%1'. Use KEY=VALUE.").arg(line);
      }
      return false;
    }
    (*env)[line.left(pos).toStdString()] = line.mid(pos + 1).toStdString();
    return true;
  });
}

// ---------------------------------------------------------------------------
// Provider model fetch (OpenAI /models endpoint)
// ---------------------------------------------------------------------------

namespace {

struct ProviderInfo {
  QString id;
  QString base_url;
  QString api_key;
};

QString ProviderModelCacheKey(const ProviderInfo& p) {
  return p.base_url + '\n' + p.api_key;
}

void ApplyFetchedProviderModels(QComboBox* comboModel, const QStringList& models) {
  comboModel->setEnabled(true);
  comboModel->clear();
  comboModel->setToolTip(QString());
  if (auto* lineEdit = comboModel->lineEdit()) {
    lineEdit->setPlaceholderText(
        models.isEmpty() ? GuiHelpers::tr("No models returned. You can type one manually.")
                         : QString());
  }
  comboModel->addItems(models);

  const QString desiredModel = comboModel->property("vinput_desired_model").toString();
  if (!desiredModel.isEmpty()) {
    comboModel->setCurrentText(desiredModel);
  }
  comboModel->setProperty("vinput_desired_model", QString());
}

void ApplyProviderModelFetchError(QComboBox* comboModel, const QString& error) {
  comboModel->setEnabled(true);
  comboModel->clear();
  comboModel->setToolTip(error);
  if (auto* lineEdit = comboModel->lineEdit()) {
    lineEdit->setPlaceholderText(
        GuiHelpers::tr("Failed to load models. Type one manually or reselect "
                       "the provider to retry."));
  }

  const QString desiredModel = comboModel->property("vinput_desired_model").toString();
  if (!desiredModel.isEmpty()) {
    comboModel->setCurrentText(desiredModel);
  }
  comboModel->setProperty("vinput_desired_model", QString());
}

void FetchModelsFromProviderAsync(const ProviderInfo& provider, QComboBox* comboModel) {
  static QHash<QString, QStringList> cache;

  if (provider.base_url.isEmpty()) {
    comboModel->setEnabled(true);
    comboModel->clear();
    comboModel->setToolTip(QString());
    if (auto* lineEdit = comboModel->lineEdit()) {
      lineEdit->setPlaceholderText(QString());
    }
    return;
  }

  const QString cacheKey = ProviderModelCacheKey(provider);
  const int generation = comboModel->property("vinput_provider_fetch_generation").toInt() + 1;
  comboModel->setProperty("vinput_provider_fetch_generation", generation);
  comboModel->setProperty("vinput_provider_fetch_key", cacheKey);

  if (cache.contains(cacheKey)) {
    ApplyFetchedProviderModels(comboModel, cache.value(cacheKey));
    return;
  }

  comboModel->setEnabled(false);
  comboModel->clear();
  comboModel->setToolTip(QString());
  if (auto* lineEdit = comboModel->lineEdit()) {
    lineEdit->setPlaceholderText(GuiHelpers::tr("Loading models..."));
  }

  auto* nam = new QNetworkAccessManager(comboModel);
  QNetworkReply* reply = StartOpenAiModelsRequest(nam, provider.base_url.toStdString(),
                                                  provider.api_key.trimmed().toStdString(),
                                                  vinput::llm::kModelFetchTimeoutMs);

  QObject::connect(
      reply, &QNetworkReply::finished, comboModel,
      [comboModel, reply, cacheKey, generation, provider_id = provider.id.toStdString()]() {
        const bool stale =
            comboModel->property("vinput_provider_fetch_generation").toInt() != generation ||
            comboModel->property("vinput_provider_fetch_key").toString() != cacheKey;

        QStringList models;
        QString error;
        bool success = false;
        if (reply->error() == QNetworkReply::NoError) {
          success = ParseOpenAiModelsResponse(reply->readAll(), &models, &error);
        } else {
          error = reply->errorString();
        }

        if (!stale) {
          if (success) {
            cache.insert(cacheKey, models);
            SaveProviderModels(provider_id, models);
            ApplyFetchedProviderModels(comboModel, models);
          } else {
            ApplyProviderModelFetchError(comboModel, error);
          }
        }

        reply->deleteLater();
        reply->manager()->deleteLater();
      });
}

// Load LLM providers from CLI -> NO, from config now.
QList<ProviderInfo> LoadLlmProviders() {
  const CoreConfig config = ConfigManager::Get().Load();
  QList<ProviderInfo> providers;
  for (const auto& prov : config.llm.providers) {
    ProviderInfo info;
    info.id = QString::fromStdString(prov.id);
    info.base_url = QString::fromStdString(prov.base_url);
    info.api_key = QString::fromStdString(prov.api_key);
    if (!info.id.isEmpty()) {
      providers.push_back(info);
    }
  }
  return providers;
}

} // namespace

void SetupProviderModelCombos(QComboBox* comboProvider, QComboBox* comboModel,
                              const QString& currentProvider, const QString& currentModel) {
  comboProvider->clear();
  comboProvider->addItem(QString()); // empty = inherit / none

  const auto providers = LoadLlmProviders();
  for (const auto& p : providers) {
    comboProvider->addItem(p.id);
  }

  comboModel->setEditable(true);
  comboModel->setProperty("vinput_desired_model", currentModel);

  auto refreshModels = [comboModel, comboProvider, providers]() {
    const QString selected = comboProvider->currentText();
    if (selected.isEmpty()) {
      comboModel->setEnabled(true);
      comboModel->clear();
      comboModel->setToolTip(QString());
      if (auto* lineEdit = comboModel->lineEdit()) {
        lineEdit->setPlaceholderText(QString());
      }
      return;
    }
    for (const auto& p : providers) {
      if (p.id == selected) {
        FetchModelsFromProviderAsync(p, comboModel);
        break;
      }
    }
  };

  QObject::connect(comboProvider, &QComboBox::activated, comboProvider, refreshModels);

  if (!currentProvider.isEmpty()) {
    comboProvider->setCurrentText(currentProvider);
    refreshModels();
  }
}

} // namespace vinput::gui
