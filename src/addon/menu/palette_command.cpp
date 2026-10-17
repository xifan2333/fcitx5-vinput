#include "menu/palette_command.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "common/utils/string_utils.h"

void PaletteCommandRegistry::registerCommand(PaletteCommand command) {
  auto it = std::find_if(commands_.begin(), commands_.end(),
                         [&](const PaletteCommand& c) { return c.id == command.id; });
  if (it != commands_.end()) {
    *it = std::move(command);
  } else {
    commands_.push_back(std::move(command));
  }
}

void PaletteCommandRegistry::clear() {
  commands_.clear();
}

const PaletteCommand* PaletteCommandRegistry::findByNameOrAlias(std::string_view token) const {
  if (token.empty()) {
    return nullptr;
  }
  for (const auto& cmd : commands_) {
    if (vinput::str::EqualsIgnoreCase(cmd.name, token) ||
        (!cmd.alias.empty() && vinput::str::EqualsIgnoreCase(cmd.alias, token))) {
      return &cmd;
    }
  }
  return nullptr;
}

const PaletteCommand* PaletteCommandRegistry::findByCategory(PaletteCategory category) const {
  for (const auto& cmd : commands_) {
    if (cmd.category == category) {
      return &cmd;
    }
  }
  return nullptr;
}
