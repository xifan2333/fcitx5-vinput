#include "daemon/audio/pipewire_control_plane.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/node.h>
#include <pipewire/proxy.h>
#include <pipewire/thread-loop.h>
#include <pipewire/type.h>
#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/audio/pipewire_device.h"
#include "common/utils/scope_guard.h"

namespace vinput::daemon::audio {

struct PipeWireControlPlane::Impl {
  pw_thread_loop* thread_loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_registry* registry = nullptr;
  pw_metadata* metadata = nullptr;
  uint32_t metadata_id = PW_ID_ANY;

  spa_hook core_listener{};
  spa_hook core_proxy_listener{};
  spa_hook registry_listener{};
  spa_hook metadata_listener{};

  int sync_seq = -1;
  bool initial_synced = false;
  std::atomic<bool> connected{false};
  std::atomic<bool> core_broken{false};

  mutable std::mutex state_mutex;
  std::map<uint32_t, vinput::pw::DeviceInfo> nodes;
  std::string default_source_name;

  void MarkCoreBroken() noexcept {
    core_broken.store(true, std::memory_order_release);
    initial_synced = true;
    if (thread_loop != nullptr) {
      pw_thread_loop_signal(thread_loop, false);
    }
  }

  void DestroyMetadataProxyLocked() noexcept {
    if (metadata == nullptr) {
      return;
    }
    spa_hook_remove(&metadata_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
    metadata = nullptr;
    metadata_id = PW_ID_ANY;
  }

  void DisconnectLocked() noexcept {
    DestroyMetadataProxyLocked();
    if (registry != nullptr) {
      spa_hook_remove(&registry_listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
      registry = nullptr;
    }
    if (core != nullptr) {
      spa_hook_remove(&core_proxy_listener);
      spa_hook_remove(&core_listener);
      pw_core_disconnect(core);
      core = nullptr;
    }
    if (context != nullptr) {
      pw_context_destroy(context);
      context = nullptr;
    }
    {
      const std::scoped_lock lock(state_mutex);
      nodes.clear();
      default_source_name.clear();
    }
    connected.store(false, std::memory_order_release);
    core_broken.store(false, std::memory_order_release);
    initial_synced = false;
    sync_seq = -1;
  }
};

namespace {

void on_cp_core_done(void* data, uint32_t id, int seq) {
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl == nullptr || id != PW_ID_CORE || seq != impl->sync_seq) {
    return;
  }
  impl->initial_synced = true;
  if (impl->thread_loop != nullptr) {
    pw_thread_loop_signal(impl->thread_loop, false);
  }
}

void on_cp_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
  (void)seq;
  (void)res;
  (void)message;
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl != nullptr && id == PW_ID_CORE) {
    impl->MarkCoreBroken();
  }
}

void on_cp_proxy_removed(void* data) {
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl != nullptr) {
    impl->MarkCoreBroken();
  }
}

void on_cp_proxy_error(void* data, int seq, int res, const char* message) {
  (void)seq;
  (void)res;
  (void)message;
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl != nullptr) {
    impl->MarkCoreBroken();
  }
}

const pw_core_events kCpCoreEvents = []() {
  pw_core_events ev{};
  spa_zero(ev);
  ev.version = PW_VERSION_CORE_EVENTS;
  ev.done = on_cp_core_done;
  ev.error = on_cp_core_error;
  return ev;
}();

const pw_proxy_events kCpProxyEvents = []() {
  pw_proxy_events ev{};
  spa_zero(ev);
  ev.version = PW_VERSION_PROXY_EVENTS;
  ev.removed = on_cp_proxy_removed;
  ev.error = on_cp_proxy_error;
  return ev;
}();

int on_cp_metadata_property(void* data, uint32_t subject, const char* key, const char* type,
                            const char* value) {
  (void)type;
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl == nullptr || subject != 0) {
    return 0;
  }
  if (key == nullptr) {
    const std::scoped_lock lock(impl->state_mutex);
    impl->default_source_name.clear();
    return 0;
  }
  if (std::string_view(key) != "default.audio.source") {
    return 0;
  }
  std::string parsed_name =
      (value != nullptr) ? vinput::pw::ParseDefaultAudioSourceMetadata(value) : std::string{};
  const std::scoped_lock lock(impl->state_mutex);
  impl->default_source_name = std::move(parsed_name);
  return 0;
}

const pw_metadata_events kCpMetadataEvents = []() {
  pw_metadata_events ev{};
  spa_zero(ev);
  ev.version = PW_VERSION_METADATA_EVENTS;
  ev.property = on_cp_metadata_property;
  return ev;
}();

void BindDefaultMetadataLocked(PipeWireControlPlane::Impl* impl, uint32_t id,
                               const spa_dict* props) {
  if (impl->metadata != nullptr || impl->registry == nullptr || impl->core == nullptr) {
    return;
  }
  const char* meta_name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
  if (meta_name == nullptr || std::string_view(meta_name) != "default") {
    return;
  }
  impl->metadata = reinterpret_cast<pw_metadata*>(
      pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
  if (impl->metadata == nullptr) {
    return;
  }
  impl->metadata_id = id;
  spa_zero(impl->metadata_listener);
  pw_metadata_add_listener(impl->metadata, &impl->metadata_listener, &kCpMetadataEvents, impl);
  impl->sync_seq = pw_core_sync(impl->core, PW_ID_CORE, 0);
}

void on_cp_registry_global(void* data, uint32_t id, uint32_t permissions, const char* type,
                           uint32_t version, const spa_dict* props) {
  (void)permissions;
  (void)version;
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl == nullptr || type == nullptr || props == nullptr) {
    return;
  }
  const std::string_view iface(type);
  if (iface == PW_TYPE_INTERFACE_Metadata) {
    BindDefaultMetadataLocked(impl, id, props);
    return;
  }
  if (iface != PW_TYPE_INTERFACE_Node) {
    return;
  }
  if (auto dev = vinput::pw::ParseRegistryNodeDevice(id, props); dev.has_value()) {
    const std::scoped_lock lock(impl->state_mutex);
    impl->nodes.insert_or_assign(id, std::move(*dev));
  }
}

void on_cp_registry_remove(void* data, uint32_t id) {
  auto* impl = static_cast<PipeWireControlPlane::Impl*>(data);
  if (impl == nullptr) {
    return;
  }
  if (impl->metadata != nullptr && impl->metadata_id == id) {
    impl->DestroyMetadataProxyLocked();
    const std::scoped_lock lock(impl->state_mutex);
    impl->default_source_name.clear();
  }
  const std::scoped_lock lock(impl->state_mutex);
  impl->nodes.erase(id);
}

const pw_registry_events kCpRegistryEvents = []() {
  pw_registry_events ev{};
  spa_zero(ev);
  ev.version = PW_VERSION_REGISTRY_EVENTS;
  ev.global = on_cp_registry_global;
  ev.global_remove = on_cp_registry_remove;
  return ev;
}();

bool AttachControlListenersLocked(PipeWireControlPlane::Impl& impl, pw_thread_loop* thread_loop) {
  impl.thread_loop = thread_loop;
  impl.context = pw_context_new(pw_thread_loop_get_loop(thread_loop), nullptr, 0);
  if (impl.context == nullptr) {
    return false;
  }
  impl.core = pw_context_connect(impl.context, nullptr, 0);
  if (impl.core == nullptr) {
    impl.DisconnectLocked();
    return false;
  }
  spa_zero(impl.core_listener);
  spa_zero(impl.core_proxy_listener);
  pw_core_add_listener(impl.core, &impl.core_listener, &kCpCoreEvents, &impl);
  pw_proxy_add_listener(reinterpret_cast<pw_proxy*>(impl.core), &impl.core_proxy_listener,
                        &kCpProxyEvents, &impl);
  impl.registry = pw_core_get_registry(impl.core, PW_VERSION_REGISTRY, 0);
  if (impl.registry == nullptr) {
    impl.DisconnectLocked();
    return false;
  }
  spa_zero(impl.registry_listener);
  pw_registry_add_listener(impl.registry, &impl.registry_listener, &kCpRegistryEvents, &impl);
  return true;
}

void WaitInitialSyncLocked(PipeWireControlPlane::Impl& impl, pw_thread_loop* thread_loop,
                           int timeout_ms) {
  impl.sync_seq = pw_core_sync(impl.core, PW_ID_CORE, 0);
  if (impl.sync_seq < 0 || timeout_ms <= 0) {
    return;
  }
  timespec abstime{};
  const int64_t timeout_ns = static_cast<int64_t>(timeout_ms) * SPA_NSEC_PER_MSEC;
  if (pw_thread_loop_get_time(thread_loop, &abstime, timeout_ns) < 0) {
    return;
  }
  while (!impl.initial_synced && !impl.core_broken.load(std::memory_order_acquire)) {
    if (pw_thread_loop_timed_wait_full(thread_loop, &abstime) == -ETIMEDOUT) {
      break;
    }
  }
}

} // namespace

PipeWireControlPlane::PipeWireControlPlane() : impl_(std::make_unique<Impl>()) {}

PipeWireControlPlane::~PipeWireControlPlane() {
  Disconnect();
}

bool PipeWireControlPlane::Connect(pw_thread_loop* thread_loop, int sync_timeout_ms) {
  if (thread_loop == nullptr) {
    return false;
  }
  pw_thread_loop_lock(thread_loop);
  const auto unlock = vinput::MakeScopeExit([thread_loop] { pw_thread_loop_unlock(thread_loop); });

  impl_->DisconnectLocked();
  if (!AttachControlListenersLocked(*impl_, thread_loop)) {
    return false;
  }
  WaitInitialSyncLocked(*impl_, thread_loop, sync_timeout_ms);
  if (impl_->core == nullptr || impl_->core_broken.load(std::memory_order_acquire)) {
    impl_->DisconnectLocked();
    return false;
  }
  impl_->connected.store(true, std::memory_order_release);
  return true;
}

bool PipeWireControlPlane::EnsureConnected(pw_thread_loop* thread_loop, int sync_timeout_ms) {
  if (impl_->connected.load(std::memory_order_acquire) &&
      !impl_->core_broken.load(std::memory_order_acquire)) {
    return true;
  }
  return Connect(thread_loop, sync_timeout_ms);
}

void PipeWireControlPlane::Disconnect() noexcept {
  pw_thread_loop* loop = impl_->thread_loop;
  if (loop == nullptr) {
    return;
  }
  pw_thread_loop_lock(loop);
  impl_->DisconnectLocked();
  impl_->thread_loop = nullptr;
  pw_thread_loop_unlock(loop);
}

pw_core* PipeWireControlPlane::Core() const noexcept {
  if (impl_->core_broken.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return impl_->core;
}

vinput::pw::ResolvedCaptureTarget
PipeWireControlPlane::ResolveTarget(std::string_view target) const {
  std::vector<vinput::pw::DeviceInfo> snapshot;
  std::string default_source;
  {
    const std::scoped_lock lock(impl_->state_mutex);
    snapshot.reserve(impl_->nodes.size());
    for (const auto& [id, dev] : impl_->nodes) {
      snapshot.push_back(dev);
    }
    default_source = impl_->default_source_name;
  }
  return vinput::pw::ResolveLiveCaptureTarget(target, snapshot, default_source);
}

} // namespace vinput::daemon::audio
