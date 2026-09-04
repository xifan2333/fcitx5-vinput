---
title: 场景与 LLM
description: 用 LLM 对识别结果进行改写、润色、翻译。
---

## 概念

ASR 输出的原始文本可能包含语气词、错别字或格式问题。**场景**定义了一套 prompt，让 **LLM** 按你的意图对文本进行改写。

```
ASR 原始文本 → [场景 prompt + LLM] → 改写后的文本
```

核心关系：

- **场景**是一组配置：prompt（指令）+ 绑定的 LLM provider + model
- **LLM 提供商**是一个 OpenAI 兼容 API 端点（如 Ollama、Groq、OpenAI）
- **LLM 适配器**是可选的本地桥接进程，把非标准的本地模型包装成 OpenAI 兼容接口，供 LLM 提供商指向

一句话：**场景决定"怎么改"，LLM 提供商决定"谁来改"，适配器解决"接口不兼容"**。

不需要 LLM 改写时，使用内置的 `__raw__` 场景，识别结果直接上屏。

## 场景

### 概念

每个场景包含：

| 字段 | 说明 |
|------|------|
| `id` | 唯一标识符 |
| `label` | 菜单中显示的名称 |
| `prompt` | 发送给 LLM 的系统提示 |
| `provider_id` | 绑定的 LLM 提供商 |
| `model` | 使用的模型名称 |
| `context_lines` | 发送给 LLM 作为上下文的前文行数 |
| `count` | LLM 改写候选最大数量（0 表示不使用 LLM） |

改写结果会自动与原始识别文本进行保序去重。去重后若只有一个结果（未作修改）直接自动上屏；若产生不同表达则弹出候选菜单供选择（默认高亮第 2 项 LLM 润色版，按 `1` 即可随时选回 ASR 原始文本）。

只有同时配置了 `provider_id` + `model` + `prompt` 的场景才会调用 LLM。

内置场景：
- **`__raw__`** — 跳过 LLM，直接输出原始识别文本
- **`__command__`** — 命令模式专用场景（见下方）

通过 `Shift_R` 在运行时切换场景。

对应配置：

```json
{
  "scenes": {
    "active_scene": "__raw__",
    "definitions": [
      {
        "id": "__raw__",
        "count": 0
      },
      {
        "id": "default",
        "label": "默认",
        "prompt": "纠正语音识别文本的错误，保留原意...",
        "provider_id": "groq",
        "model": "openai/gpt-oss-120b",
        "context_lines": 3,
        "count": 5
      }
    ]
  }
}
```

### GUI 操作

在 Vinput GUI 的 **LLM** tab 中管理场景：添加、编辑 prompt、绑定 provider 和 model，并设置 **Context Lines**。

`context_lines` 用来控制 Vinput 在改写前额外发送给 LLM 的前文行数。它适合翻译、续写、段落级润色等依赖附近文本语境的场景。设为 `0` 表示不附带额外上下文。

### CLI 操作

```bash
vinput scene list               # 列出所有场景
vinput scene add --id <id>      # 添加场景
vinput scene edit <id>          # 编辑场景
vinput scene use <id>           # 切换当前场景
vinput scene remove <id>        # 删除场景
```

添加带 LLM 的场景时，`--provider`、`--model`、`--prompt` 需要同时提供。

## LLM 提供商

### 概念

LLM 提供商是一个 OpenAI 兼容的 API 端点。场景通过 `provider_id` 引用它。

你可以配置多个提供商，不同场景可以绑定不同的提供商。

对应配置：

```json
{
  "llm": {
    "providers": [
      {
        "id": "groq",
        "base_url": "https://api.groq.com/openai/v1",
        "api_key": "your-api-key"
      }
    ]
  }
}
```

### 配置示例

以本地 [Ollama](https://ollama.com) 为例：

```bash
vinput llm add ollama --base-url http://127.0.0.1:11434/v1
vinput scene add --id polish \
  --label "润色" \
  --provider ollama \
  --model qwen2.5:7b \
  --context-lines 3 \
  --prompt "将识别结果润色为自然的中文。"
vinput scene use polish
```

### CLI 操作

```bash
vinput llm list                         # 列出已配置提供商
vinput llm add <id> --base-url <url>    # 添加
vinput llm edit <id> --base-url <url>   # 编辑
vinput llm remove <id>                  # 删除
```

## LLM 适配器

### 概念

如果你想用的本地模型不直接提供 OpenAI 兼容接口，可以安装一个 **LLM 适配器**。适配器是一个本地进程，把模型包装成 OpenAI 兼容 API，然后你在 LLM 提供商中把 `base_url` 指向它。

对应配置：

```json
{
  "llm": {
    "adapters": []
  }
}
```

### GUI 操作

在 Vinput GUI 中进入 **资源 → LLM 适配器**，浏览并安装。
在 **控制** 页面中管理适配器的启动、停止与是否伴随守护进程自启动。

### CLI 操作

```bash
vinput adapter ls               # 列出已安装适配器及自启策略
vinput adapter list -a          # 浏览可用远程适配器
vinput adapter ps               # 查看适配器进程运行状态与 PID
vinput adapter status <id>      # 查看指定适配器的详细状态
vinput adapter add <id>         # 安装适配器
vinput adapter start <id>       # 启动适配器进程
vinput adapter stop <id>        # 停止适配器进程
vinput adapter restart <id>     # 重启适配器进程
vinput adapter enable <id>      # 设为随 daemon 自启动
vinput adapter disable <id>     # 取消随 daemon 自启动
```

## 命令模式

### 概念

命令模式是一种特殊的场景用法：选中已有文本，用语音指令让 LLM 改写它。

操作流程：选中文本 → 按住 `Control_R` → 说出指令 → 松开 → 完成。

底层使用内置的 `__command__` 场景。默认 prompt 模板通过 `{{selected}}` 和 `{{asr}}` 接收选中文本与语音指令，并用 Vinput 专用 XML 标签（`<vinput-selected>` 和 `<vinput-asr>`）隔离数据。

**示例：**
- 选中中文 → 说 *"翻译成英文"* → 替换为英文译文
- 选中代码 → 说 *"加上注释"* → 替换为加注释版本

如果当前没有 surrounding-text 选区，会回退到 primary selection 剪贴板内容。

> 命令模式需要先配置 LLM 提供商，并在 `__command__` 场景中绑定 `provider_id` 和 `model`。
