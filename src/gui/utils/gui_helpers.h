#pragma once

#include <QComboBox>
#include <QStringList>
#include <QTableWidgetItem>
#include <QWidget>

namespace vinput::gui {

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
