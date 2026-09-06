---
title: VINPUT-GUI
section: 1
header: fcitx5-vinput 用户手册
footer: fcitx5-vinput
...

# 名称
vinput-gui - fcitx5-vinput 图形化配置与资源管理界面

# 语法
**vinput-gui**

# 描述
**vinput-gui** 是 **fcitx5-vinput** 的 Qt 桌面图形配置工具。

它提供了直观的可视化界面用于：

- **控制与音频设置**：选择 PipeWire 麦克风录音设备、调节输入增益、开关 VAD（静音检测）以及管理守护进程服务状态。
- **资源管理**：浏览、下载、激活与卸载离线语音识别模型（**sherpa-onnx**）、云端 ASR 服务商脚本及 LLM 适配器脚本。
- **大模型与场景配置**：配置 OpenAI 兼容的 LLM 服务商参数、编写自定义提示词模板（Prompt）、设置附带上下文行数以及管理文本润色场景。
- **热词管理**：管理用于提升本地模型识别准确率的专有名词与自定义词表。

# 文件
*~/.config/vinput/config.json*
:   守护进程核心配置文件。

*~/.config/fcitx5/conf/vinput.conf*
:   Fcitx5 插件配置文件。

*/usr/share/applications/vinput-gui.desktop*
:   桌面启动快捷方式文件。

# 另请参阅
**vinput**(1), **vinput-daemon**(1), **vinput-config**(5), **fcitx5**(1)
