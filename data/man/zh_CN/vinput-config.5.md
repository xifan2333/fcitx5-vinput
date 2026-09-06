---
title: VINPUT-CONFIG
section: 5
header: fcitx5-vinput 文件格式
footer: fcitx5-vinput
...

# 名称
vinput-config - fcitx5-vinput 配置文件格式与选项说明

# 描述
**fcitx5-vinput** 使用两份主要的配置文件：

1. *~/.config/vinput/config.json* — 核心配置，涵盖音频采集、ASR 引擎、LLM 服务商及后处理场景。
2. *~/.config/fcitx5/conf/vinput.conf* — Fcitx5 插件配置，涵盖快捷键及界面显示设置。

# CONFIG.JSON 结构

核心配置文件采用 JSON 格式，主要包含四个顶级字段：**global**、**asr**、**llm** 与 **scenes**。

## GLOBAL 全局配置
控制底层音频环境与录音行为。

```json
"global": {
  "default_language": "zh",
  "capture_device": "default",
  "duck_output_while_recording": false,
  "duck_output_volume": 0.25
}
```

**default_language** (*字符串*)
:   默认语言代码（例如 `"zh"`, `"en"`）。

**capture_device** (*字符串*)
:   PipeWire 音频采集节点名称，或 `"default"` 使用系统默认输入设备。

**duck_output_while_recording** (*布尔值*)
:   是否在录音时通过 WirePlumber 自动降低系统扬声器音量。

**duck_output_volume** (*浮点数*, `0.0` - `1.0`)
:   降低音量时的目标音量比例（`0.25` 表示降至原音量的 25%）。

## ASR 语音识别配置
配置本地与云端语音识别引擎。

```json
"asr": {
  "active_provider": "sherpa-onnx",
  "normalize_audio": true,
  "input_gain": 1.0,
  "vad": {
    "enabled": true,
    "threshold": 0.5,
    "min_speech_duration": 0.15,
    "speech_pad_ms": 300
  },
  "providers": [ ... ]
}
```

**active_provider** (*字符串*)
:   当前激活的 ASR 服务商 ID。本地离线识别使用 `"sherpa-onnx"`。

**normalize_audio** (*布尔值*)
:   是否在识别前对输入音频波形进行归一化。

**input_gain** (*浮点数*)
:   软件录音增益倍数。

**vad.enabled** (*布尔值*)
:   是否启用静音检测，自动过滤首尾无效静音。

## LLM 服务商配置
定义 OpenAI 兼容的 API 端点。

```json
"llm": {
  "providers": [
    {
      "id": "groq",
      "base_url": "https://api.groq.com/openai/v1",
      "api_key": "YOUR_API_KEY",
      "extra_body": {}
    }
  ],
  "adapters": [
    {
      "id": "my-adapter",
      "command": "python3",
      "args": ["/path/to/entry.py"],
      "auto_start": true
    }
  ]
}
```

**adapters[].auto_start** (*布尔值*)
:   是否在 **vinput-daemon** 启动时自动启动该适配器进程。

## SCENES 场景后处理配置
定义用于文字润色、错别字修正或翻译的提示词模板与候选词配置。

```json
"scenes": {
  "active_scene": "__raw__",
  "definitions": [
    {
      "id": "__raw__",
      "count": 0
    },
    {
      "id": "default",
      "label": "默认润色",
      "prompt": "修正拼写与标点，保持原意不变。",
      "provider_id": "groq",
      "model": "llama-3.3-70b-versatile",
      "context_lines": 3,
      "count": 5
    }
  ]
}
```

**active_scene** (*字符串*)
:   当前激活的场景 ID。`__raw__` 表示跳过 LLM 后处理，直接输出 ASR 原始文本。

**context_lines** (*整数*)
:   发送给 LLM 的历史上下文行数（`0` 表示不附带上下文）。

**count** (*整数*, `0` - `9`)
:   LLM 最大条目数。向 LLM 请求改写候选的最大数量（设为 `0` 表示不调用 LLM）。改写结果与原始识别文本自动保序去重。去重后若只有单一结果直接自动上屏；若有不同候选则弹出菜单，默认聚焦第 1 项（当 `raw_cand` 为 true 时为原始识别结果，后续各项为改写候选）。

**raw_cand** (*布尔值*, 默认 `true`)
:   是否在候选词列表中包含原始语音文本。关闭（`false`）时候选词数组仅包含 LLM/适配器结果，在单结果时可直接上屏。

**raw_prev** (*布尔值*, 默认 `true`)
:   是否在后处理等待期间于浮动面板中预览原始语音文本并支持回车提前上屏。关闭（`false`）时后处理期间保持紧凑单行状态，不展开原句辅助框，消除极速适配器在桌面环境下的弹窗闪烁。

# VINPUT.CONF 结构 (FCITX5 插件配置)

Fcitx5 插件使用 INI 格式的配置文件 *~/.config/fcitx5/conf/vinput.conf*。

```ini
TriggerMode=Both
HoldActivationDelay=300
MaxStreamingDisplayWidth=60

[TriggerKey]
0=Alt_R
[CommandKeys]
0=Control_R
[MenuKey]
0=Shift_R
[PagePrevKeys]
0=Page_Up
1=KP_Page_Up
[PageNextKeys]
0=Page_Down
1=KP_Page_Down
```

**TriggerMode** (*枚举值: Tap, Hold, Both*)
:   `Tap` 点按切换录音；`Hold` 按住说话、松开停止；`Both` 同时支持这两种方式。

    单个或多个修饰键组成的快捷键（如 `Control_R`、`Control+Shift_L`），点按时在全部松开后触发。按住修饰键快捷键期间，按下其他键会取消本次操作，并丢弃此次长按已开始的录音。

    单修饰键快捷键会占用该键的按下和松开；多修饰键组合仍将按键传给应用，供单独使用各修饰键，但应用也可能响应同一组合。其他按键保留修饰键状态（如 `Ctrl+C`）；鼠标点击不会取消快捷键。

**HoldActivationDelay** (*整数*, `100` - `2000`)
:   长按判定时长，单位毫秒（默认：`300`）。在 Hold 模式下设置激活延迟，在 Both 模式下区分点按与长按。

**TriggerKey** (*按键列表*)
:   触发语音录音的快捷键（默认：右 Alt）。

**CommandKeys** (*按键列表*)
:   对选中文本执行划词语音指令的按键（默认：右 Control）。

**MenuKey** (*按键列表*)
:   打开统一命令面板的快捷键（默认：右 Shift）。纯修饰键快捷键点按后打开，其他快捷键按下时打开。

**MaxStreamingDisplayWidth** (*整数*, `0` - `500`)
:   实时流式识别预览的最大视觉显示列宽。超出时自动将较早文字折叠为“句首...句尾”。设为 `0` 则禁用折叠（默认：`60`）。

# 文件
*~/.config/vinput/config.json*
:   用户核心配置文件。

*~/.config/fcitx5/conf/vinput.conf*
:   Fcitx5 插件快捷键与界面配置文件。

*~/.var/app/org.fcitx.Fcitx5/config/vinput/config.json*
:   在 Flatpak 沙箱环境下的配置文件路径。

# 另请参阅
**vinput**(1), **vinput-daemon**(1), **fcitx5**(1)
