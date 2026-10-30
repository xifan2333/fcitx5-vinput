#include "cli/config/fcitx_controller.h"

#include <algorithm>
#include <charconv>
#include <fcitx-config/dbushelper.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/message.h>
#include <fcitx-utils/dbus/variant.h>
#include <fcitx-utils/event.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config/vinput_config.h"
#include "common/runtime/runtime_defaults.h"

namespace vinput::cli {

namespace {

constexpr const char kFcitxBusName[] = "org.fcitx.Fcitx5";
constexpr const char kFcitxControllerPath[] = "/controller";
constexpr const char kFcitxControllerInterface[] = "org.fcitx.Fcitx.Controller1";
constexpr const char kAddonConfigUri[] = "fcitx://config/addon/vinput";
constexpr const char kMethodGetConfig[] = "GetConfig";
constexpr const char kMethodSetConfig[] = "SetConfig";

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

std::string ReplyError(const char* method, const fcitx::dbus::Message& reply) {
  return std::string(method) + " failed: " + reply.errorName() + ": " + reply.errorMessage();
}

class EventLoopDetacher {
public:
  explicit EventLoopDetacher(fcitx::dbus::Bus& bus) : bus_(bus) {}
  ~EventLoopDetacher() { bus_.detachEventLoop(); }
  EventLoopDetacher(const EventLoopDetacher&) = delete;
  EventLoopDetacher& operator=(const EventLoopDetacher&) = delete;
  EventLoopDetacher(EventLoopDetacher&&) = delete;
  EventLoopDetacher& operator=(EventLoopDetacher&&) = delete;

private:
  fcitx::dbus::Bus& bus_;
};

} // namespace

TriggerKeyUpdate AppendTriggerKeys(fcitx::RawConfig* raw,
                                   const std::vector<std::string>& key_names) {
  TriggerKeyUpdate update;
  if (raw == nullptr || key_names.empty()) {
    return update;
  }

  const auto trigger = raw->get("TriggerKey", true);
  if (!trigger) {
    return update;
  }

  std::vector<std::string> existing;
  int next_index = 0;
  for (const auto& child_name : trigger->subItems()) {
    const auto child = trigger->get(child_name);
    if (!child) {
      continue;
    }
    existing.push_back(child->value());
    int parsed = 0;
    const auto* begin = child_name.data();
    const auto* end = begin + child_name.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec == std::errc{} && result.ptr == end) {
      next_index = std::max(next_index, parsed + 1);
    }
  }

  for (const auto& name : key_names) {
    if (name.empty() || std::ranges::find(existing, name) != existing.end()) {
      continue;
    }
    const std::string index = std::to_string(next_index++);
    trigger->setValueByPath(index, name);
    existing.push_back(name);
    update.entries.push_back({.index = index, .name = name});
  }
  return update;
}

bool BindFcitxTriggerKeys(const std::vector<std::string>& key_names, TriggerKeyUpdate* update,
                          std::string* error) {
  if (update != nullptr) {
    *update = {};
  }
  if (error != nullptr) {
    error->clear();
  }
  if (key_names.empty()) {
    return true;
  }

  fcitx::EventLoop loop;
  fcitx::dbus::Bus bus(fcitx::dbus::BusType::Session);
  if (!bus.isOpen()) {
    SetError(error, "fcitx5 is not running (session D-Bus unavailable)");
    return false;
  }
  // Do not let D-Bus activation start fcitx5 as a side effect of selecting a
  // capture device. `GetNameOwner` reports whether it is already running.
  if (bus.serviceOwner(kFcitxBusName, vinput::runtime::kDbusCallTimeoutUsec).empty()) {
    SetError(error, "fcitx5 is not running");
    return false;
  }
  bus.attachEventLoop(&loop);
  const EventLoopDetacher detacher(bus);

  auto get = bus.createMethodCall(kFcitxBusName, kFcitxControllerPath, kFcitxControllerInterface,
                                  kMethodGetConfig);
  get << std::string(kAddonConfigUri);
  auto reply = get.call(vinput::runtime::kDbusCallTimeoutUsec);
  if (reply.type() == fcitx::dbus::MessageType::Error) {
    SetError(error, ReplyError(kMethodGetConfig, reply));
    return false;
  }
  if (!reply) {
    SetError(error, "GetConfig returned no reply");
    return false;
  }

  fcitx::dbus::Variant variant;
  reply >> variant;
  if (!reply) {
    SetError(error, "cannot decode the fcitx5 addon configuration");
    return false;
  }

  fcitx::RawConfig raw = fcitx::variantToRawConfig(variant);
  auto change = AppendTriggerKeys(&raw, key_names);
  if (!change.Empty()) {
    auto set = bus.createMethodCall(kFcitxBusName, kFcitxControllerPath, kFcitxControllerInterface,
                                    kMethodSetConfig);
    set << std::string(kAddonConfigUri) << fcitx::rawConfigToVariant(raw);
    const auto set_reply = set.call(vinput::runtime::kDbusCallTimeoutUsec);
    if (set_reply.type() == fcitx::dbus::MessageType::Error) {
      SetError(error, ReplyError(kMethodSetConfig, set_reply));
      return false;
    }
    if (!set_reply) {
      SetError(error, "SetConfig returned no reply");
      return false;
    }
  }

  if (update != nullptr) {
    *update = std::move(change);
  }
  return true;
}

TriggerKeyUpdate PlannedTriggerKeysFromDisk(const std::vector<std::string>& key_names) {
  fcitx::RawConfig raw;
  fcitx::readAsIni(raw, kVinputConfigPath);
  return AppendTriggerKeys(&raw, key_names);
}

} // namespace vinput::cli
