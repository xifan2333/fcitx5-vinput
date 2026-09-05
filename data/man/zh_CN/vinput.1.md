---
title: VINPUT
section: 1
header: fcitx5-vinput 用户手册
footer: fcitx5-vinput
date: 2024年8月
...

# 名称
vinput - fcitx5-vinput 命令行控制与管理工具

# 语法
**vinput** [*全局选项*] *命令* [*子命令*] [*选项*]

# 描述
**vinput** 是 **fcitx5-vinput** 的命令行客户端。**fcitx5-vinput** 是为 Fcitx5 输入法框架开发的离线/云端双模语音输入插件。

**vinput** 提供了对离线语音识别模型、云端 ASR 脚本、LLM 后处理提供商及适配器、识别场景、音频采集设备、热词表、守护进程生命周期以及录音状态的完整控制。

# 全局选项
**-h**, **--help**
:   显示帮助信息并退出。

**-v**, **--version**
:   显示程序版本并退出。

**-j**, **--json**
:   以 JSON 格式输出结果，便于脚本解析。

# 命令

## 初始化
**init** [**-f**, **--force**]
:   初始化默认配置文件及相关目录（创建于 *~/.config/vinput* 和 *~/.local/share/vinput*）。若指定 **-f**，则会覆盖已有配置。

## 本地模型管理
管理基于 **sherpa-onnx** 的离线语音识别模型（在 Lite 轻量版中不可用）。

**model list** [**-a**, **--available**]
:   列出本地已安装的模型。若附加 **-a** 或 **--available**，则从在线注册表检索可用模型列表。

**model add** *ID*
:   从注册表下载并安装指定短 *ID* 的离线模型。

**model use** *ID*
:   将指定模型设为当前激活的离线 ASR 模型。

**model remove** *ID*
:   卸载指定的本地模型。

**model info** *ID*
:   查看指定模型的详细元数据与配置信息。

## 云端 ASR 服务商管理
管理第三方云端语音识别脚本（如豆包、阿里云百炼、ElevenLabs、OpenAI 兼容接口等）。

**provider list** [**-a**, **--available**]
:   列出本地已配置的 ASR 服务商。若指定 **-a**，则浏览在线注册表中可用的服务商脚本。

**provider add** *ID*
:   从注册表安装指定 *ID* 的服务商脚本。

**provider use** *ID*
:   将激活的 ASR 服务商切换为 *ID*。

**provider edit** *ID*
:   在默认文本编辑器中打开该服务商的配置或脚本。

**provider remove** *ID*
:   卸载指定的 ASR 服务商。

## LLM 润色服务商管理
管理用于场景后处理、错别字修正和文字润色的 OpenAI 兼容大模型接口。

**llm list**
:   列出所有已配置的 LLM 服务商。

**llm add** *ID* **-u**, **--base-url** *URL* [**-k**, **--api-key** *KEY*] [**-e**, **--extra-body** *JSON*]
:   添加新的 LLM 服务商，指定 Base URL、API Key（可选）及额外的请求体参数（可选）。

**llm edit** *ID* [**-u**, **--base-url** *URL*] [**-k**, **--api-key** *KEY*] [**-e**, **--extra-body** *JSON*]
:   修改已有 LLM 服务商的连接配置。

**llm remove** *ID*
:   删除指定的 LLM 服务商。

**llm test** *ID*
:   测试与指定 LLM 服务商的网络连通性及 API 认证是否有效。

## LLM 适配器管理
管理将非标准本地模型封装为 OpenAI 兼容接口的本地桥接适配进程。

**adapter list** [**-a**, **--available**]
:   列出本地适配器（显示自启动状态）；使用 **-a** 浏览在线注册表中的可用适配器。

**adapter ps**
:   列出适配器进程与运行时状态（包括运行状态、PID、自启动策略与启动命令）。

**adapter status** *ID*
:   查看指定适配器的详细运行状态与配置属性。

**adapter add** *ID*
:   安装指定 *ID* 的适配器脚本。

**adapter start** *ID*
:   启动后台适配器进程。

**adapter stop** *ID*
:   停止正在运行的适配器进程。

**adapter restart** *ID*
:   重启指定的适配器进程。

**adapter enable** *ID*
:   将指定适配器设置为随守护进程自启动。

**adapter disable** *ID*
:   取消指定适配器随守护进程自启动。

## 热词表管理
管理用于增强离线 sherpa-onnx 模型专有名词识别准确率的自定义词表（在 Lite 轻量版中不可用）。

**hotword get**
:   显示当前生效的热词文件路径。

**hotword set** *PATH*
:   设置热词文本文件路径。

**hotword clear**
:   清除已配置的热词文件路径。

**hotword edit**
:   在默认文本编辑器中编辑热词文件。

## 音频设备管理
**device list**
:   列出系统中可用的 PipeWire 录音设备。

**device use** *ID*
:   设置当前用于录音的输入设备（使用设备节点名称或 *default*）。

## 场景管理
场景（Scene）定义了发送给 LLM 的提示词模板（Prompt）及相关参数，用于对 ASR 识别出的原始文本进行重写与润色。

**scene list**
:   列出所有已定义的场景。

**scene add** **--id** *ID* [**-l**, **--label** *LABEL*] [**-t**, **--prompt** *PROMPT*] [**-p**, **--provider** *PROVIDER_ID*] [**-m**, **--model** *MODEL*] [**-c**, **--count** *N*] [**--show-raw** [*布尔值*]]
:   添加新的后处理场景。*N* 表示向 LLM 请求的不同改写结果最大数量；`0` 表示该场景不使用 LLM。传入 **--show-raw false** 可在候选列表中排除原始语音转写文本，从而在单改写结果时直接上屏。

**scene edit** *ID* [*选项*]
:   修改已有场景配置。

**scene use** *ID*
:   切换当前激活的场景。

**scene remove** *ID*
:   删除场景（`__raw__` 等内置基础场景不可删除）。

## 配置读写
**config get** *POINTER*
:   通过 JSON Pointer 路径读取配置值（例如 `/global/capture_device` 或 `/asr/input_gain`）。

**config set** *POINTER* [*VALUE*] [**-i**, **--stdin**]
:   写入指定 JSON Pointer 路径的配置值。

**config edit** *TARGET*
:   在 `$EDITOR` 中打开配置文件。*TARGET* 必须为 **core**（*~/.config/vinput/config.json*）或 **fcitx**（*~/.config/fcitx5/conf/vinput.conf*）。

## 守护进程控制
**daemon status**
:   查看 **vinput-daemon** 的运行状态（PID、活动模型、D-Bus 连接等）。

**daemon start**
:   通过 systemd user 服务启动守护进程。

**daemon stop**
:   停止守护进程。

**daemon restart**
:   重启守护进程。

## 录音控制
**recording start**
:   开始录音。

**recording stop** [**-s**, **--scene** *SCENE_ID*]
:   结束录音，触发语音识别并按指定场景进行后处理。

**recording toggle** [**-s**, **--scene** *SCENE_ID*]
:   切换录音状态（开始/停止）。

# 文件
*~/.config/vinput/config.json*
:   核心守护进程、ASR、LLM 与场景配置文件。

*~/.config/fcitx5/conf/vinput.conf*
:   Fcitx5 输入法插件配置文件（按键快捷键、候选词数量、界面设置等）。

*~/.local/share/vinput/models/*
:   已安装的离线 sherpa-onnx 模型存放目录。

*~/.local/share/vinput/providers/*
:   已安装的云端 ASR 服务商脚本目录。

*~/.local/share/vinput/adapters/*
:   已安装的 LLM 适配器脚本目录。

# 另请参阅
**vinput-daemon**(1), **vinput-config**(5), **fcitx5**(1), **systemctl**(1)
