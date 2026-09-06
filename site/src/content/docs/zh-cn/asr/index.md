---
title: 语音识别
description: 语音识别：本地模型、云端提供商与热词。
---

## 概念

ASR（Automatic Speech Recognition）负责将语音转换为文字，是整个语音输入流程的第一步。

```
麦克风 → [ASR] → 原始文本 → （可选）场景 + LLM 改写 → 最终文本
```

Vinput 提供三种互相配合的 ASR 机制：

- **本地模型** — 基于 [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 的离线识别，不依赖网络，隐私友好，延迟低
- **云端提供商** — 调用第三方云 ASR 接口（豆包、阿里百炼、ElevenLabs、OpenAI 等），识别效果通常更好，但需要网络和 API Key
- **热词** — 为本地模型补充领域特定词汇，提升专有名词的识别准确率（部分模型支持）

本地模型和云端提供商是**二选一**的关系，通过右 Shift 命令面板的 `/asr` 在运行时随时切换。热词在本地模型激活时生效。

## 本地模型

### 概念

本地模型是一组 sherpa-onnx 兼容的模型文件，安装后完全离线运行。每个模型有自己的语言、类型和大小，一次只能激活一个。

对应配置：

```json
{
  "asr": {
    "providers": [
      {
        "id": "sherpa-onnx",
        "type": "local",
        "model": "model.sherpa-onnx.sense-voice-zh-en-ja-ko-yue-int8",
        "timeout_ms": 15000
      }
    ]
  }
}
```

### GUI 操作

在 Vinput GUI 中进入 **资源 → 模型**：

- **可用模型**列表：点击 **下载** 安装模型
- **已安装模型**列表：点击 **使用** 激活，**移除** 卸载

### CLI 操作

```bash
vinput model list               # 列出已安装模型
vinput model list -a            # 列出可用远程模型
vinput model add <名称>          # 下载安装
vinput model use <名称>          # 激活
vinput model remove <名称>       # 卸载
vinput model info <名称>         # 查看详情
```

## 云端提供商

### 概念

云端提供商是一个外部脚本，接收音频流，调用第三方 ASR API，返回识别文本。每个提供商有自己的环境变量配置（API Key、URL 等）。

提供商分两种模式：
- **非流式** — 录音结束后一次性发送音频，等待完整结果
- **流式** — 边录边识别，实时返回中间结果

对应配置：

```json
{
  "asr": {
    "active_provider": "provider.doubaoime.streaming",
    "providers": [
      {
        "id": "provider.bailian.streaming",
        "type": "command",
        "command": "python3",
        "args": ["~/.local/share/vinput/providers/bailian/streaming"],
        "env": {
          "VINPUT_ASR_API_KEY": "your-api-key",
          "VINPUT_ASR_MODEL": "qwen3-asr-flash-realtime"
        },
        "timeout_ms": 60000
      }
    ]
  }
}
```

### GUI 操作

在 Vinput GUI 中进入 **资源 → ASR 提供商**：

- 点击 **安装** 下载提供商脚本
- 安装后在 **控制** 页面选择并编辑提供商的环境变量（如 API Key）

### CLI 操作

```bash
vinput provider list -a        # 列出可用远程提供商
vinput provider add <id>       # 安装
vinput provider use <id>       # 切换为当前提供商
vinput provider edit <id>      # 编辑配置（环境变量）
vinput provider remove <id>    # 卸载
```

### 可用提供商

| 提供商 | 模式 | 说明 |
|--------|------|------|
| 豆包（非流式） | 非流式 | 豆包语音 / 火山引擎录音文件极速版 |
| ElevenLabs | 非流式 / 流式 | ElevenLabs speech-to-text API |
| 阿里百炼 | 非流式 / 流式 | Qwen3-ASR，OpenAI 兼容 / Realtime API |
| 豆包（流式） | 流式 | 火山引擎 AI Gateway Doubao ASR Realtime |
| 豆包输入法（流式） | 流式 | 非官方豆包输入法实时语音识别协议 |
| OpenAI 兼容 | 非流式 / 流式 | OpenAI `/v1/audio/transcriptions` 或 Realtime WebSocket |

## 热词

### 概念

热词是一个文本文件，每行一个词条，用于提升本地模型对特定词汇的识别率。典型场景：人名、品牌名、专业术语。

```text
OpenAI
语音识别:2.0
deep learning:3.5
```

对于支持逐词权重的模型，在词条末尾直接追加 `:<权重>`。冒号两边都不能有空格：应写成 `词条:2.0`，不能写成 `词条 :2.0` 或 `词条: 2.0`。不支持逐词权重的模型会忽略权重，只使用词条内容。

并非所有模型都支持热词，模型列表中会标注是否支持。

### GUI 操作

在 Vinput GUI 中进入 **热词** tab 进行编辑。

### CLI 操作

```bash
vinput hotword get              # 查看当前热词文件路径
vinput hotword set <路径>        # 设置热词文件
vinput hotword edit             # 用编辑器打开热词文件
vinput hotword clear            # 清除热词配置
```
