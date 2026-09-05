#include "menu/palette_command.h"

#include <algorithm>
#include <cctype>

namespace {

std::string ToLower(std::string_view str) {
  std::string res;
  res.reserve(str.size());
  for (char ch : str) {
    res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return res;
}

} // namespace

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
  const std::string lower = ToLower(token);
  for (const auto& cmd : commands_) {
    if (ToLower(cmd.name) == lower || (!cmd.alias.empty() && ToLower(cmd.alias) == lower)) {
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
