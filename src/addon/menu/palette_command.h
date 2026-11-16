#pragma once

#include <cstdint>
#include <fcitx/inputcontext.h>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/registry/registry_i18n.h"

class VinputEngine;

enum class PaletteCategory : std::uint8_t {
  Asr,
  Scene,
  CommandModel,
  Process,
  Custom,
};

struct PaletteItem {
  PaletteCategory category = PaletteCategory::Scene;
  std::string id;
  std::string text;
  std::string comment;
  std::string search_text;
  bool active = false;
  std::string provider_id;
  std::string model_id;
  std::string scene_id;
  std::string adapter_id;
  bool is_command_entry = false;
  std::string command_token;
};

struct PaletteCommand {
  std::string id;
  std::string name;
  std::string alias;
  std::string title;
  std::string comment;
  std::string description;
  PaletteCategory category = PaletteCategory::Custom;

  std::function<std::vector<PaletteItem>(VinputEngine* engine)> getItems;
  std::function<void(VinputEngine* engine, const PaletteItem& item, fcitx::InputContext* ic)>
      onSelect;
};

class PaletteCommandRegistry {
public:
  void registerCommand(PaletteCommand command);
  void clear();
  [[nodiscard]] const PaletteCommand* findByNameOrAlias(std::string_view token) const;
  [[nodiscard]] const PaletteCommand* findByCategory(PaletteCategory category) const;
  [[nodiscard]] const std::vector<PaletteCommand>& commands() const { return commands_; }

private:
  std::vector<PaletteCommand> commands_;
};

PaletteItem BuildClientPaletteItem(const ClientSpec& client, bool is_trigger, bool running,
                                   pid_t pid, const vinput::registry::I18nMap& i18n_map = {});
