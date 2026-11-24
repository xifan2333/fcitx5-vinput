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
{"type":"final","text":"已确认的识别文本","segment_final":true,"utterance_final":true}
{"type":"error","message":"..."}
{"type":"closed"}
```

语义说明：
- `partial.text` — 当前时刻用户应看到的完整文本
- `final.text` — 当前时刻已确认的完整文本
- `segment_final` — 可选布尔值，表示分段确认（`finish` 后不会提前结束整句等待）
- `utterance_final` — 可选布尔值，表示整句最终识别结果已完成
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

适配器是一个标准的 one-shot 命令行管道过滤器，用于通过本地脚本或模型改写语音识别文本或生成候选。

**运行时约定：**

| 项目 | 规格 |
|------|------|
| 命令 | 解释器或可执行文件（如 `python3`） |
| 运行方式 | 每次改写请求启动一次 one-shot 子进程 |
| `stdin` | 待改写文本（普通场景为原始字符串，命令模式为 JSON） |
| `stdout` | 改写后的候选结果（纯文本或 JSON） |
| `stderr` | 用户提示信息，直接转为桌面系统通知 |
| 依赖 | 仅标准库（禁止第三方包） |

### 候选输出格式（`stdout`）

普通场景改写与命令模式共用相同的候选解析逻辑。适配器可以通过以下任意一种格式向 `stdout` 输出改写候选：

1. **JSON 格式（推荐）**：
   包含 `candidates` 字符串数组的 JSON 对象（支持被 ````json` 等 markdown 代码块包裹）：

   ```json
   {
     "candidates": [
       "候选结果 1",
       "候选结果 2"
     ]
   }
   ```

2. **多行纯文本**：
   每个非空行作为一个独立的候选结果。

3. **单行纯文本**：
   去除首尾空白后的整段文本直接作为唯一候选。

### 输入格式（`stdin`）

- **普通场景改写**：
  ASR 识别出的原始待改写文本字符串。

- **命令模式改写**：
  包含语音指令与当前选中文本的 JSON 对象：

  ```json
  {
    "text": "将这句翻译为英文",
    "selected": "你好世界"
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
