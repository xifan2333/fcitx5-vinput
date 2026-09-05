#include "utils/gui_helpers.h"

#include <QApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <vector>

#include "common/llm/defaults.h"
#include "common/llm/provider_model_cache.h"
#include "common/utils/url_utils.h"

#include "gui/utils/config_manager.h"

namespace vinput::gui {

namespace {
struct GuiHelpers {
  Q_DECLARE_TR_FUNCTIONS(GuiHelpers)
};
} // namespace

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
  if (!data.isEmpty())
    cell->setData(Qt::UserRole, data);
  return cell;
}

bool ValidateProviderInput(const QString& name, const QString& base_url, QString* error_out) {
  if (name.trimmed().isEmpty()) {
    if (error_out) {
      *error_out = GuiHelpers::tr("Provider name must not be empty.");
    }
    return false;
  }

  const QUrl url = QUrl::fromUserInput(base_url.trimmed());
  if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty() ||
      (url.scheme() != "http" && url.scheme() != "https")) {
    if (error_out) {
      *error_out = GuiHelpers::tr("Base URL must be a valid http:// or https:// URL.");
    }
    return false;
  }

  return true;
}

bool ParseCommandEnv(const QString& text, std::map<std::string, std::string>* env,
                     QString* error_out) {
  if (!env) {
    return false;
  }

  env->clear();
  for (const QString& line : NonEmptyLines(text)) {
    const int pos = line.indexOf('=');
    if (pos <= 0) {
      if (error_out) {
        *error_out = GuiHelpers::tr("Invalid env entry '%1'. Use KEY=VALUE.").arg(line);
      }
      return false;
    }
    (*env)[line.left(pos).toStdString()] = line.mid(pos + 1).toStdString();
  }
  return true;
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

  const std::string url_str =
      vinput::url::JoinPath(provider.base_url.toStdString(), vinput::llm::kOpenAiModelsPath);
  QString url = QString::fromStdString(url_str);

  auto* nam = new QNetworkAccessManager(comboModel);
  QNetworkRequest req{QUrl(url)};
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  if (!provider.api_key.trimmed().isEmpty()) {
    const QByteArray bearer =
        QByteArray(vinput::llm::kBearerPrefix) + provider.api_key.trimmed().toUtf8();
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
  timeout->start(vinput::llm::kModelFetchTimeoutMs);

  QObject::connect(
      reply, &QNetworkReply::finished, comboModel,
      [comboModel, reply, timeout, cacheKey, generation,
       provider_id = provider.id.toStdString()]() {
        timeout->stop();

        const bool stale =
            comboModel->property("vinput_provider_fetch_generation").toInt() != generation ||
            comboModel->property("vinput_provider_fetch_key").toString() != cacheKey;

        QStringList models;
        QString error;
        bool success = false;
        if (reply->error() == QNetworkReply::NoError) {
          QJsonParseError parseError;
          const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
          if (parseError.error != QJsonParseError::NoError || !doc.isObject() ||
              !doc.object().value("data").isArray()) {
            error = GuiHelpers::tr("Provider returned invalid JSON for /v1/models.");
          } else {
            const QJsonArray data = doc.object().value("data").toArray();
            for (const auto& v : data) {
              const QString id = v.toObject().value("id").toString();
              if (!id.isEmpty()) {
                models.append(id);
              }
            }
            models.removeDuplicates();
            models.sort();
            success = true;
          }
        } else {
          error = reply->errorString();
        }

        if (!stale) {
          if (success) {
            cache.insert(cacheKey, models);
            std::vector<std::string> models_vec;
            models_vec.reserve(models.size());
            for (const auto& m : models) {
              models_vec.push_back(m.toStdString());
            }
            vinput::llm::SaveProviderModels(provider_id, models_vec);
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
  CoreConfig config = ConfigManager::Get().Load();
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
    QString selected = comboProvider->currentText();
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

  QObject::connect(comboProvider, QOverload<int>::of(&QComboBox::activated), comboProvider,
                   refreshModels);

  if (!currentProvider.isEmpty()) {
    comboProvider->setCurrentText(currentProvider);
    refreshModels();
  }
}

} // namespace vinput::gui
