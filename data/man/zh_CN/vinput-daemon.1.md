---
title: VINPUT-DAEMON
section: 1
header: fcitx5-vinput 用户手册
footer: fcitx5-vinput
...

# 名称
vinput-daemon - fcitx5-vinput 后台语音识别与处理守护进程

# 语法
**vinput-daemon** [*选项*]

# 选项

**--log-level**=*LEVEL*
:   设置日志过滤门槛：`trace`、`debug`、`info`、`warn`、`error`、`critical`、`off`（默认值：`info`）。亦可通过环境变量 `VINPUT_LOG_LEVEL` 设置。

# 描述
**vinput-daemon** 是 **fcitx5-vinput** 的后台核心服务组件，通过 D-Bus（`org.fcitx.Vinput`）与 Fcitx5 插件及 CLI 工具通信。

守护进程的主要职责包括：

- 通过 **PulseAudio** / **pipewire-pulse**（`libpulse`）捕获麦克风与系统内录音频；
- 基于 **sherpa-onnx** 进行本地离线语音识别（ASR）；
- 支持流式及批处理转发至云端 ASR 服务商脚本；
- 基于 **VAD**（静音检测）过滤无效音频；
- 调度 OpenAI 兼容大模型进行场景后处理与重写；
- 管理后台 LLM 适配器进程的生命周期；
- 录音期间通过 **libpulse** 动态降低系统默认输出音量（Audio Ducking）。

# 服务管理与生命周期

**vinput-daemon** 作为 systemd 用户服务运行，亦支持 D-Bus 按需激活。

启动服务:
:   `systemctl --user start vinput-daemon.service`

设置登录自启并立即运行:
:   `systemctl --user enable --now vinput-daemon.service`

查看运行日志:
:   `journalctl --user -u vinput-daemon.service -f`

# 环境变量

**VINPUT_LOG_LEVEL**
:   日志过滤门槛：`trace`、`debug`、`info`、`warn`、`error`、`critical`、`off`（默认值：`info`）。排查问题时可设置为 `debug` 或 `trace`。

**VINPUT_CONFIG_PATH**
:   自定义 `config.json` 的配置文件路径。

# 隐私与音频安全

空闲状态下，守护进程仅保持轻量控制连接，**绝不会打开录音流或向内存缓冲任何麦克风音频**；录音结束或中止时，采集流会立即断开销毁。

# 文件
*~/.config/vinput/config.json*
:   守护进程核心配置文件。

*/usr/share/systemd/user/vinput-daemon.service*
:   Systemd 用户服务单元文件。

*/usr/share/dbus-1/services/org.fcitx.Vinput.service*
:   D-Bus 激活配置文件。

# 另请参阅
**vinput**(1), **vinput-config**(5), **fcitx5**(1), **systemctl**(1), **pulseaudio**(1)
