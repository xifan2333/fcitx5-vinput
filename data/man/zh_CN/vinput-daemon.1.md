---
title: VINPUT-DAEMON
section: 1
header: fcitx5-vinput 用户手册
footer: fcitx5-vinput
...

# 名称
vinput-daemon - fcitx5-vinput 后台语音识别与处理守护进程

# 语法
**vinput-daemon**

# 描述
**vinput-daemon** 是 **fcitx5-vinput** 的后台核心服务组件，通过 D-Bus（`org.fcitx.Vinput`）与 Fcitx5 插件及 CLI 工具通信。

守护进程的主要职责包括：

- 通过 **PipeWire** 捕获麦克风输入音频；
- 基于 **sherpa-onnx** 进行本地离线语音识别（ASR）；
- 支持流式及批处理转发至云端 ASR 服务商脚本；
- 基于 **VAD**（静音检测）过滤无效音频；
- 调度 OpenAI 兼容大模型进行场景后处理与重写；
- 管理后台 LLM 适配器进程的生命周期；
- 录音期间通过 **WirePlumber** 动态降低系统背景音量（Audio Ducking）。

# 服务管理与生命周期

**vinput-daemon** 作为 systemd 用户服务运行，亦支持 D-Bus 按需激活。

启动服务:
:   `systemctl --user start vinput-daemon.service`

设置登录自启并立即运行:
:   `systemctl --user enable --now vinput-daemon.service`

查看运行日志:
:   `journalctl --user -u vinput-daemon.service -f`

# 环境变量

**VINPUT_CAPTURE_REUSE**
:   控制录音结束后是否在短时间内保持已连接但未激活的 PipeWire 音频流（默认值：`1`）。该热路径（Warm Path）机制显著降低了下一次触发录音时的启动延迟。设置为 `0` 则每次停止录音后立即销毁音频流。

**VINPUT_CAPTURE_IDLE_DESTROY_MS**
:   闲置超时时间（毫秒），超时后销毁未激活的 PipeWire 音频流（默认值：`15000`，即 15 秒）。

**VINPUT_DEBUG**
:   设置为 `1` 可开启详细的调试日志输出。

**VINPUT_CONFIG_PATH**
:   自定义 `config.json` 的配置文件路径。

# 隐私与音频安全

当音频热路径（Warm Path）处于非活跃状态时，**绝不会读取或向内存缓冲任何音频数据**。闲置期过后，音频采集流将被完全关闭与销毁。

# 文件
*~/.config/vinput/config.json*
:   守护进程核心配置文件。

*/usr/share/systemd/user/vinput-daemon.service*
:   Systemd 用户服务单元文件。

*/usr/share/dbus-1/services/org.fcitx.Vinput.service*
:   D-Bus 激活配置文件。

# 另请参阅
**vinput**(1), **vinput-config**(5), **fcitx5**(1), **systemctl**(1), **pipewire**(1)
