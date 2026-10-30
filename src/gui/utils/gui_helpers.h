#pragma once

#include <QComboBox>
#include <QStringList>
#include <QTableWidgetItem>
#include <QWidget>
#include <atomic>

namespace vinput::script {
struct ResourceVersionInfo;
}

namespace vinput::gui {

// Check if a background worker cancellation token has been signalled.
[[nodiscard]] inline bool IsWorkerCancelled(const std::atomic<bool>* cancel_token) noexcept {
  return cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed);
}

// Format user-facing status string for a registry resource (provider or adapter).
QString FormatScriptUpdateStatus(const vinput::script::ResourceVersionInfo& ver_info,
                                 bool installed);

// Split text into non-empty trimmed lines.
QStringList NonEmptyLines(const QString& text);

// Create a read-only table cell, optionally with UserRole data.
QTableWidgetItem* MakeCell(const QString& text, const QString& data = {});

// Validate LLM provider input (name + base_url).
bool ValidateProviderInput(const QString& name, const QString& base_url, QString* error_out);

// Parse KEY=VALUE lines into a map. Returns false on error.
bool ParseCommandEnv(const QString& text, std::map<std::string, std::string>* env,
                     QString* error_out);

// Populate provider combo from `vinput -j llm list`, wire it to refresh
// model combo via OpenAI /models endpoint.
void SetupProviderModelCombos(QComboBox* comboProvider, QComboBox* comboModel,
                              const QString& currentProvider = {},
                              const QString& currentModel = {});

} // namespace vinput::gui
