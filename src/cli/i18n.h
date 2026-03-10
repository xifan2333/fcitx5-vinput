#pragma once

struct Msg {
  const char* en;
  const char* zh;
};

namespace msgs {
  // General
  constexpr Msg kErrorPrefix = {"Error", "错误"};
  constexpr Msg kWarningPrefix = {"Warning", "警告"};
  constexpr Msg kSuccess = {"Success", "成功"};
  constexpr Msg kNotImplemented = {"Not implemented yet.", "尚未实现。"};
  // Model
  constexpr Msg kModelListHeader = {"MODELS", "模型列表"};
  constexpr Msg kActive = {"Active", "活跃"};
  constexpr Msg kInstalled = {"Installed", "已安装"};
  constexpr Msg kBroken = {"Broken", "损坏"};
  constexpr Msg kDownloading = {"Downloading %s...", "正在下载 %s..."};
  constexpr Msg kVerifying = {"Verifying checksum...", "正在校验..."};
  constexpr Msg kExtracting = {"Extracting...", "正在解压..."};
  constexpr Msg kInstallSuccess = {"Model '%s' installed successfully.", "模型 '%s' 安装成功。"};
  // Scene
  constexpr Msg kSceneListHeader = {"SCENES", "场景列表"};
  // LLM
  constexpr Msg kLlmListHeader = {"LLM PROVIDERS", "LLM 提供商"};
  // Config
  constexpr Msg kConfigValueSet = {"Config value set.", "配置值已设置。"};
  // LLM
  constexpr Msg kLlmProviderAdded = {"LLM provider '%s' added.", "LLM 提供商 '%s' 已添加。"};
  constexpr Msg kLlmProviderSwitched = {"Active LLM provider set to '%s'.", "活跃 LLM 提供商已设为 '%s'。"};
  constexpr Msg kLlmProviderRemoved = {"LLM provider '%s' removed.", "LLM 提供商 '%s' 已移除。"};
  constexpr Msg kLlmProviderNotFound = {"LLM provider '%s' not found.", "LLM 提供商 '%s' 未找到。"};
  constexpr Msg kLlmCannotRemoveActive = {"Cannot remove active provider '%s'. Use --force.", "不能移除活跃提供商 '%s'，请用 --force。"};
  constexpr Msg kLlmProviderExists = {"LLM provider '%s' already exists.", "LLM 提供商 '%s' 已存在。"};
  // Daemon
  constexpr Msg kDaemonNotRunning = {"Daemon is not running.", "守护进程未运行。"};
  constexpr Msg kDaemonStatus = {"Daemon status: %s", "守护进程状态：%s"};
  constexpr Msg kDaemonStarted = {"Daemon started.", "守护进程已启动。"};
  constexpr Msg kDaemonStopped = {"Daemon stopped.", "守护进程已停止。"};
  constexpr Msg kDaemonRestarted = {"Daemon restarted.", "守护进程已重启。"};
}

inline const char* _(const Msg& m, bool chinese) {
  return chinese ? m.zh : m.en;
}
