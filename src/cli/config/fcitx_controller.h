#pragma once

#include <fcitx-config/rawconfig.h>
#include <string>
#include <vector>

namespace vinput::cli {

struct TriggerKeyEntry {
  std::string index;
  std::string name;
};

struct TriggerKeyUpdate {
  std::vector<TriggerKeyEntry> entries;

  [[nodiscard]] bool Empty() const noexcept { return entries.empty(); }
};

// Appends missing keys to `[TriggerKey]` in `raw`, preserving existing
// entries and using the next free numeric indices. Pure; no I/O.
TriggerKeyUpdate AppendTriggerKeys(fcitx::RawConfig* raw,
                                   const std::vector<std::string>& key_names);

// Appends keys through `org.fcitx.Fcitx.Controller1` GetConfig/SetConfig.
// Returns false with `error` when fcitx5 is unavailable or rejects the
// update; the addon configuration is not modified in that case.
bool BindFcitxTriggerKeys(const std::vector<std::string>& key_names, TriggerKeyUpdate* update,
                          std::string* error);

// Computes the entries a user must add manually from the on-disk addon
// configuration. Does not write any file.
TriggerKeyUpdate PlannedTriggerKeysFromDisk(const std::vector<std::string>& key_names);

} // namespace vinput::cli
