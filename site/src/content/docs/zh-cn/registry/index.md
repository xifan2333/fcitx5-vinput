---
title: 资源仓库
description: "vinput-registry 贡献规范：模型、提供商和适配器。"
---

## 概述

[vinput-registry](https://github.com/xifan2333/vinput-registry) 是一个独立仓库，托管 Vinput 所有可下载资源：本地 ASR 模型、云端 ASR 提供商脚本和 LLM 适配器脚本。当你在 GUI 中点击 **下载** / **安装**，或执行 `vinput model add` / `vinput provider add` / `vinput adapter add` 时，资源就是从这个仓库拉取的。

```
vinput-registry/
├── registry/
│   ├── models.json          # 本地 ASR 模型索引
│   ├── providers.json       # 云端 ASR 提供商索引
│   └── adapters.json        # LLM 适配器索引
├── i18n/
│   ├── en_US.json           # 英文显示文本
│   └── zh_CN.json           # 中文显示文本
└── resources/
    ├── providers/<folder>/<variant>/
    │   ├── entry.py
    │   └── README.md
    └── adapters/<folder>/<name>/
        ├── entry.py
        └── README.md
```

## 核心规则

- 所有脚本必须**自包含**，只能使用所选运行时的标准库。禁止引入第三方依赖，不能有 `pip install`、`npm install` 等额外安装步骤。这样可以保证脚本在任何人的机器上都能直接运行。
- 脚本可以使用任何语言。推荐 Python 3，因为几乎所有 Linux 发行版都自带。Node.js、Bash 等运行时也可以，只要满足"仅标准库"的约束。
- registry JSON 中的 `command` 字段类似 VS Code `tasks.json`：指定解释器，`args` 指定脚本路径。
- 下载 URL 使用数组形式，按顺序回退。
- 每个脚本资源必须包含入口脚本和 `README.md`。

## ID 规范

| 类型 | 格式 | 示例 |
|------|------|------|
| 模型 | `model.sherpa-onnx.<模型名>` | `model.sherpa-onnx.sense-voice-zh-en-ja-ko-yue-int8` |
| 提供商 | `provider.<目录>.<变体>` | `provider.bailian.streaming` |
| 适配器 | `adapter.<目录>.<名称>` | `adapter.mtranserver.proxy` |

- `id` 是**稳定的机器标识符**，发布后不可更改
- `short_id` 仅用于 CLI/GUI/日志的人类可读显示
- 显示文本存储在 `i18n/*.json` 中，格式为 `<id>.title` 和 `<id>.description`

## ASR 提供商脚本

### 非流式（batch）

非流式提供商接收完整录音，返回识别文本。

**运行时约定：**

| 项目 | 规格 |
|------|------|
| 命令 | `python3` |
| 输入 | 裸 PCM `S16_LE`，单声道，`16000 Hz`，通过 **stdin**（二进制） |
| 输出 | 最终识别文本，通过 **stdout** |
| 错误 | 可读错误信息，通过 **stderr** |
| 退出码 | `0` 成功，`1` 运行时错误，`2` 用法错误 |
| 依赖 | 仅标准库（禁止第三方包） |

**文件结构：**

```
resources/providers/<目录>/batch/
├── entry.py
└── README.md
```

### 流式（streaming）

流式提供商边录边识别，实时返回中间结果。

**运行时约定：**

| 项目 | 规格 |
|------|------|
| 命令 | `python3` |
| 输入 | JSONL，通过 **stdin** |
| 输出 | JSONL，通过 **stdout** |
| 错误 | 仅 **stderr** |
| 退出码 | `0` 成功，`1` 运行时错误，`2` 用法错误 |
| 依赖 | 仅标准库（禁止第三方包） |

**输入协议（stdin）：**

```json
{"type":"audio","audio_base64":"<base64 PCM S16_LE 单声道 16kHz>"}
{"type":"finish"}
{"type":"cancel"}
```

**输出协议（stdout）：**

```json
{"type":"session_started","session_id":"..."}
{"type":"partial","text":"当前部分识别文本"}
{"type":"final","text":"已确认的识别文本","segment_final":true}
{"type":"error","message":"..."}
{"type":"closed"}
```

语义说明：
- `partial.text` — 当前时刻用户应看到的完整文本
- `final.text` — 当前时刻已确认的完整文本
- 脚本负责累计和去重 segment

**文件结构：**

```
resources/providers/<目录>/streaming/
├── entry.py
└── README.md
```

### 环境变量

提供商环境变量使用 `VINPUT_ASR_*` 命名空间。通用变量：

| 变量 | 用途 |
|------|------|
| `VINPUT_ASR_API_KEY` | Bearer 风格 API 凭证 |
| `VINPUT_ASR_APP_ID` | 应用标识符 |
| `VINPUT_ASR_ACCESS_TOKEN` | 令牌凭证（非 API Key 风格） |
| `VINPUT_ASR_URL` | 完整端点覆盖 |
| `VINPUT_ASR_BASE_URL` | 基础端点（脚本拼接最终路径） |
| `VINPUT_ASR_MODEL` | 远程模型标识符 |
| `VINPUT_ASR_LANGUAGE` | 语言提示 |
| `VINPUT_ASR_PROMPT` | 识别 prompt 或偏置文本 |
| `VINPUT_ASR_TIMEOUT` | 网络超时（秒） |
| `VINPUT_ASR_FINISH_GRACE_SECS` | `finish` 后额外等待时间 |
| `VINPUT_ASR_ENABLE_VAD` | 启用服务端 VAD |
| `VINPUT_ASR_VAD_THRESHOLD` | VAD 灵敏度 |
| `VINPUT_ASR_VAD_PREFIX_PADDING_MS` | 语音前音频填充 |
| `VINPUT_ASR_VAD_SILENCE_DURATION_MS` | 静音关闭时长 |

上游 API 特有的变量允许使用，但仍需使用 `VINPUT_ASR_*` 前缀。

## LLM 适配器脚本

适配器是一个本地进程，把非标准的 LLM 包装成 OpenAI 兼容 API。

**运行时约定：**

| 项目 | 规格 |
|------|------|
| 命令 | 解释器（如 `python3`） |
| 提供 | 本地 HTTP 服务 |
| 必须实现 | `GET /v1/models`、`POST /v1/chat/completions` |
| 依赖 | 仅标准库（禁止第三方包） |

### 响应格式

对于 `POST /v1/chat/completions`，Vinput 要求：

- 仅非流式：`"stream": false`
- 标准 OpenAI chat completion JSON 响应结构
- `choices[0].message.content` 必须是字符串
- 该字符串本身必须是如下格式的 JSON：

```json
{
  "candidates": [
    "候选结果 1",
    "候选结果 2"
  ]
}
```

外层是 OpenAI 兼容响应，内层 `content` 字符串是 Vinput 消费的结构化数据。

`GET /v1/models` 只需返回标准 OpenAI 列表格式：

```json
{
  "object": "list",
  "data": [
    {
      "id": "my-model",
      "object": "model",
      "owned_by": "my-adapter"
    }
  ]
}
```

**文件结构：**

```
resources/adapters/<目录>/<名称>/
├── entry.py
└── README.md
```

适配器环境变量使用各自的前缀（如 `MTRAN_*`），不使用 `VINPUT_ASR_*`。

## 本地 ASR 模型

`models.json` 中的模型条目描述可下载的 sherpa-onnx 模型归档。

**必填字段：**

| 字段 | 说明 |
|------|------|
| `id` | 稳定 ID，如 `model.sherpa-onnx.<名称>` |
| `short_id` | 人类可读短 ID |
| `urls` | 下载 URL 数组（按回退顺序） |
| `sha256` | 归档校验和 |
| `size_bytes` | 归档大小 |
| `language` | `zh`、`en`、`multilingual` 等 |
| `vinput_model` | 运行时元数据（见下方） |

**`vinput_model` 结构：**

| 字段 | 说明 |
|------|------|
| `backend` | `sherpa-offline` 或 `sherpa-streaming` |
| `runtime` | `offline` 或 `online` |
| `family` | sherpa-onnx C API 族：`dolphin`、`sense_voice`、`paraformer`、`transducer`、`qwen3_asr` |
| `language` | 语言代码 |
| `size_bytes` | 模型大小 |
| `supports_hotwords` | 是否支持热词 |
| `recognizer` | sherpa-onnx recognizer 配置（采样率、解码方法等） |
| `model` | sherpa-onnx model 配置（tokens 文件、族特定模型路径） |

字段命名遵循 sherpa-onnx C API 约定。

## 贡献资源

### 添加提供商

1. 创建 `resources/providers/<目录>/<batch|streaming>/entry.py` + `README.md`
2. 在 `registry/providers.json` 中添加条目
3. 在 `i18n/en_US.json` 和 `i18n/zh_CN.json` 中添加显示文本

### 添加适配器

1. 创建 `resources/adapters/<目录>/<名称>/entry.py` + `README.md`
2. 在 `registry/adapters.json` 中添加条目
3. 添加 i18n 条目

### 添加模型

1. 在 `registry/models.json` 中添加条目，包含完整 `vinput_model` 元数据
2. 添加 i18n 条目

### i18n 条目格式

```json
{
  "<资源ID>.title": "显示名称",
  "<资源ID>.description": "一行描述。"
}
```

`en_US.json` 和 `zh_CN.json` 都需要更新。
