# 项目结构与架构建议（整理版）

本文档基于当前代码结构，整理出项目结构层面的主要问题与改进建议，并补充
“单步 LLM、无管道”的后处理方向说明。

## 后处理方向（意图识别 + 分流）

目标：把语音当作自然触发器，在录音结束后先做一次意图识别与分类，再决定
进入“命令执行”或“上屏/对话”分支。整体只新增一步，不改变现有上屏逻辑。

建议流程（新增步骤以 **粗体** 标注）：

1. 录音结束 → ASR 得到初始文本与基础置信度  
2. **LLM/规则意图识别：判定 Action Type**  
3. 分流  
   - Command：执行前弹出确认/取消 → 执行 → 结果提示  
   - Chat：沿用原有上屏/对话后处理逻辑

### Action Type 约定（建议）

为简化后处理，类型只保留两类：

- `command`：需要执行动作（如打开浏览器搜索、启动应用、系统操作等）
- `chat`：纯文本流，进入原有上屏/对话流程（选词、候选、直接上屏等）

### “技能库 + 场景”的配置方向（JSON）

目标：把常用指令抽象成“技能”，由场景选择性启用，再在 `command`
分支做“技能识别与触发”。这样 `command` 不必细分，但依然可控与可复用。

建议思路：

1. 技能库（全局/公共）  
   - 每个技能描述：名称、触发语义（示例/规则/向量）、参数模板、执行器  
   - 例如：`git_commit_message`、`web_search`、`open_app`
   - 可包含“全局文本类技能”（如自动纠错、润色）

2. 场景（支持继承默认场景）  
   - 选择启用哪些技能（白名单）
   - 可覆盖技能参数默认值（如搜索引擎、仓库路径、语言等）
   - 默认场景 `default` 作为基类，其他场景继承其启用技能与默认参数

3. 运行时  
   - `command` 分支先做“技能识别”  
   - 命中技能 → 参数抽取 → 生成执行计划 → 确认/取消 → 执行
   - 未命中技能 → 回退到 `chat` 或提示用户改写

这样“场景 = 技能集合 + 默认参数”，可以支持你说的“一个场景里包含多种常用动作”。

### 最小 JSON 草案（仅文档示意）

`skills.json`（公共技能库）：

```json
{
  "schema_version": 1,
  "skills": [
    {
      "id": "git_commit_message",
      "name": "Generate Git Commit Message",
      "triggers": ["帮我写提交信息", "生成 commit message"],
      "params": {
        "language": "en",
        "style": "conventional"
      },
      "executor": "local/llm_commit_message"
    },
    {
      "id": "web_search",
      "name": "Web Search",
      "triggers": ["帮我搜索", "打开谷歌搜索"],
      "params": {
        "engine": "google"
      },
      "executor": "local/open_browser"
    }
  ]
}
```

`scene.json`（场景启用清单 + 默认参数覆盖，支持继承）：

```json
{
  "schema_version": 1,
  "scenes": [
    {
      "id": "default",
      "name": "Default",
      "enabled_skills": [
        {
          "id": "web_search",
          "params": { "engine": "google" }
        }
      ]
    },
    {
      "id": "dev_daily",
      "name": "Developer Daily",
      "extends": "default",
      "enabled_skills": [
        {
          "id": "git_commit_message",
          "params": { "language": "en", "style": "short" }
        }
      ]
    }
  ]
}
```

运行时：`command` → 计算场景技能集合（继承合并） → 技能识别（在合并后 `enabled_skills` 中匹配）
→ 参数抽取/合并 → 生成执行计划 → 确认/取消 → 执行。

### Command 执行最小安全闭环

- 必须二次确认（确认/取消）
- 记录可审计的执行结果（成功/失败 + 原因）
- 失败可回落为 `chat` 或提示用户改写

### 意图识别默认回落规则

当无法高置信度判定为 `command` 时，默认归类为 `chat`，
保持“像在说话”的体验，直接走文本流动流程。

### 影响范围

- 现有后处理流程仅新增“意图识别与分类”这一步
- 原有“上屏/对话”流程不需要改动
- CLI/GUI 解耦可以在此流程稳定后推进

## TODO 列表（截至 2026-03-11）

已修复：1 / 8

按优先级排序（仅未完成项，文档相关后置）：

1. [P1] [ ] 

[x] 配置频繁加载，缺少快照/缓存  
建议：daemon 维护配置快照，并通过文件变更触发刷新。

2. [P1] [ ] GUI 过度依赖 CLI  
建议：提供 DBus 或共享库 API，让 GUI 直接读取。

3. [P2] [ ]（搁置：后处理逻辑）RecognitionResult 协议缺少版本化  
建议：JSON schema + `schema_version`，向后兼容。

4. [P2] [ ]（搁置：场景）CLI 回归：`scene add` 误将 `--type` 变成必填  
建议：先设置默认值为 `input`，再做合法性校验，或在校验时允许空值。

5. [P3] [ ]（搁置：场景）场景交互逻辑不清晰  
说明：先落地“意图识别 + 分流”流程，再补充场景规则与交互细节。

6. [P3] [ ]（文档）模型规范与 README 不一致  
建议：统一模型规范为 “目录 + vinput-model.json + files”，并同步更新 README。

7. [P3] [ ]（文档）支持的模型类型范围不一致  
建议：文档明确“支持/实验”范围，CLI/GUI 显示模型类型能力。
