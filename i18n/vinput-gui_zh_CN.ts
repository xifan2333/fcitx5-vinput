<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN">
<context>
    <name>MainWindow</name>
    <message>
        <source>Vinput Configuration</source>
        <translation>Vinput 配置</translation>
    </message>
    <message>
        <source>Save Settings</source>
        <translation>保存设置</translation>
    </message>
    <message>
        <source>Hotwords</source>
        <translation>热词</translation>
    </message>
    <message>
        <source>Success</source>
        <translation>成功</translation>
    </message>
    <message>
        <source>Settings saved successfully!</source>
        <translation>设置已成功保存！</translation>
    </message>
    <message>
        <source>Control</source>
        <translation>控制</translation>
    </message>
    <message>
        <source>Resources</source>
        <translation>资源</translation>
    </message>
    <message>
        <source>LLM</source>
        <translation>LLM</translation>
    </message>
    <message>
        <source>Open Config</source>
        <translation>打开配置</translation>
    </message>
    <message>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <source>Failed to save config.</source>
        <translation>保存配置失败。</translation>
    </message>
    <message>
        <source>Details</source>
        <translation>查看详情</translation>
    </message>
    <message>
        <source>Start with daemon</source>
        <translation>随守护进程启动</translation>
    </message>
</context>
<context>
    <name>vinput::gui::ControlPage</name>
    <message>
        <source>&lt;b&gt;Audio&lt;/b&gt;</source>
        <translation>&lt;b&gt;音频&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Capture Device:</source>
        <translation>录音设备：</translation>
    </message>
    <message>
        <source>Normalize audio</source>
        <translation>归一化音频</translation>
    </message>
    <message>
        <source>Apply peak normalization to captured audio before recognition.</source>
        <translation>在识别前对采集到的音频做峰值归一化。</translation>
    </message>
    <message>
        <source>Input gain:</source>
        <translation>输入增益：</translation>
    </message>
    <message>
        <source>Microphone gain multiplier applied to captured audio.</source>
        <translation>应用于采集音频的麦克风增益倍数。</translation>
    </message>
    <message>
        <source>Trim silence (VAD)</source>
        <translation>裁剪静音（VAD）</translation>
    </message>
    <message>
        <source>Use voice activity detection to trim leading/trailing silence.</source>
        <translation>使用语音活动检测裁剪首尾静音。</translation>
    </message>
    <message>
        <source>Reduce output volume while recording</source>
        <translation>录音时降低输出音量</translation>
    </message>
    <message>
        <source>Lower the system output volume while recording, then restore it. Useful when speaker sound leaks into the microphone.</source>
        <translation>录音时降低系统输出音量，结束后恢复。适用于外放声音被麦克风收入、干扰识别的情况。</translation>
    </message>
    <message>
        <source>Output volume while recording:</source>
        <translation>录音时的输出音量：</translation>
    </message>
    <message>
        <source>Fraction of the current output volume to keep while recording.</source>
        <translation>录音时保留的当前输出音量比例。</translation>
    </message>
    <message>
        <source> %</source>
        <translation> %</translation>
    </message>
    <message>
        <source>&lt;b&gt;ASR Providers&lt;/b&gt;</source>
        <translation>&lt;b&gt;ASR 提供商&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Edit</source>
        <translation>编辑</translation>
    </message>
    <message>
        <source>Remove</source>
        <translation>移除</translation>
    </message>
    <message>
        <source>Activate</source>
        <translation>激活</translation>
    </message>
    <message>
        <source>Daemon Status:</source>
        <translation>守护进程状态：</translation>
    </message>
    <message>
        <source>Unknown</source>
        <translation>未知</translation>
    </message>
    <message>
        <source>Start</source>
        <translation>启动</translation>
    </message>
    <message>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message>
        <source>Restart</source>
        <translation>重启</translation>
    </message>
    <message>
        <source>Edit ASR Provider</source>
        <translation>编辑 ASR 提供商</translation>
    </message>
    <message>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <source>Confirm</source>
        <translation>确认</translation>
    </message>
    <message>
        <source>Are you sure you want to remove ASR provider &apos;%1&apos;?</source>
        <translation>确定要移除 ASR 提供商&quot;%1&quot;吗？</translation>
    </message>
    <message>
        <source>Stopped</source>
        <translation>已停止</translation>
    </message>
    <message>
        <source>Running (Status Error: %1)</source>
        <translation>运行中（状态错误：%1）</translation>
    </message>
    <message>
        <source>Running: %1</source>
        <translation>运行中：%1</translation>
    </message>
    <message>
        <source>Running: %1 (loading backend)</source>
        <translation>运行中：%1（后端加载中）</translation>
    </message>
    <message>
        <source>Running: %1 (backend error)</source>
        <translation>运行中：%1（后端错误）</translation>
    </message>
    <message>
        <source>Additional Install Required</source>
        <translation>需要额外配置</translation>
    </message>
    <message>
        <source>Vinput requires additional Flatpak permissions. Run the following commands, then restart Fcitx5:

%1</source>
        <translation>Vinput 需要额外的 Flatpak 权限。请运行以下命令，然后重启 Fcitx5：

%1</translation>
    </message>
    <message>
        <source>Failed to save config.</source>
        <translation>保存配置失败。</translation>
    </message>
    <message>
        <source>Warning</source>
        <translation>警告</translation>
    </message>
    <message>
        <source>Config saved, but failed to reload ASR backend: %1</source>
        <translation>配置已保存，但重新加载 ASR 后端失败：%1</translation>
    </message>
    <message>
        <source>configured</source>
        <translation>已配置</translation>
    </message>
    <message>
        <source>effective</source>
        <translation>已生效</translation>
    </message>
    <message>
        <source>loading</source>
        <translation>加载中</translation>
    </message>
    <message>
        <source>reload failed</source>
        <translation>重载失败</translation>
    </message>
    <message>
        <source>The local ASR provider cannot be removed.</source>
        <translation>内置的本地语音识别不可移除。</translation>
    </message>
    <message>
        <source>&lt;b&gt;Adapters&lt;/b&gt;</source>
        <translation>&lt;b&gt;适配器&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Start with daemon</source>
        <translation>随守护进程启动</translation>
    </message>
    <message>
        <source>Automatically start this adapter when vinput-daemon starts.</source>
        <translation>当 vinput-daemon 启动时自动启动此适配器。</translation>
    </message>
    <message>
        <source>autostart</source>
        <translation>自启动</translation>
    </message>
    <message>
        <source>manual</source>
        <translation>手动</translation>
    </message>
    <message>
        <source>Command: %1</source>
        <translation>命令：%1</translation>
    </message>
    <message>
        <source>Args: %1</source>
        <translation>参数：%1</translation>
    </message>
    <message>
        <source>Env: %1</source>
        <translation>环境变量：%1</translation>
    </message>
</context>
<context>
    <name>vinput::gui::HotwordPage</name>
    <message>
        <source>Path to hotwords file...</source>
        <translation>热词文件路径...</translation>
    </message>
    <message>
        <source>Browse...</source>
        <translation>浏览...</translation>
    </message>
    <message>
        <source>Hotwords (one per line, optional transducer score: &quot;word:2.0&quot;):</source>
        <translation>热词（每行一个，可选 Transducer 权重：&quot;词:2.0&quot;）：</translation>
    </message>
    <message>
        <source>Select Hotwords File</source>
        <translation>选择热词文件</translation>
    </message>
    <message>
        <source>Text Files (*.txt);;All Files (*)</source>
        <translation>文本文件 (*.txt);;所有文件 (*)</translation>
    </message>
</context>
<context>
    <name>vinput::gui::LlmPage</name>
    <message>
        <source>&lt;b&gt;Providers&lt;/b&gt;</source>
        <translation>&lt;b&gt;提供商&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Add</source>
        <translation>添加</translation>
    </message>
    <message>
        <source>Edit</source>
        <translation>编辑</translation>
    </message>
    <message>
        <source>Remove</source>
        <translation>移除</translation>
    </message>
    <message>
        <source>LLM adapters are local OpenAI-compatible bridge processes. Install them from the Resources page, then manage runtime here.</source>
        <translation>LLM 适配器是本地 OpenAI 兼容桥接进程。从资源页面安装后，可在此管理运行状态。</translation>
    </message>
    <message>
        <source>Start</source>
        <translation>启动</translation>
    </message>
    <message>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message>
        <source>Refresh</source>
        <translation>刷新</translation>
    </message>
    <message>
        <source>&lt;b&gt;Installed Adapters&lt;/b&gt;</source>
        <translation>&lt;b&gt;已安装适配器&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;b&gt;Scenes&lt;/b&gt;</source>
        <translation>&lt;b&gt;场景&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Activate</source>
        <translation>激活</translation>
    </message>
    <message>
        <source>Command: %1</source>
        <translation>命令：%1</translation>
    </message>
    <message>
        <source>Args: %1</source>
        <translation>参数：%1</translation>
    </message>
    <message>
        <source>Env: %1</source>
        <translation>环境变量：%1</translation>
    </message>
    <message>
        <source>Add LLM Provider</source>
        <translation>添加 LLM 提供商</translation>
    </message>
    <message>
        <source>Name:</source>
        <translation>名称：</translation>
    </message>
    <message>
        <source>Base URL:</source>
        <translation>地址：</translation>
    </message>
    <message>
        <source>API Key:</source>
        <translation>密钥：</translation>
    </message>
    <message>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <source>Edit LLM Provider</source>
        <translation>编辑 LLM 提供商</translation>
    </message>
    <message>
        <source>Leave empty to keep current key</source>
        <translation>留空保持当前密钥不变</translation>
    </message>
    <message>
        <source>Confirm</source>
        <translation>确认</translation>
    </message>
    <message>
        <source>Are you sure you want to remove LLM provider &apos;%1&apos;?</source>
        <translation>确定要移除 LLM 提供商&quot;%1&quot;吗？</translation>
    </message>
    <message>
        <source>Adapter &apos;%1&apos; not found in configuration.</source>
        <translation>配置中未找到适配器&quot;%1&quot;。</translation>
    </message>
    <message>
        <source>LLM Adapter Started</source>
        <translation>LLM 适配器已启动</translation>
    </message>
    <message>
        <source>Adapter &apos;%1&apos; started.</source>
        <translation>适配器&quot;%1&quot;已启动。</translation>
    </message>
    <message>
        <source>Scenes Updated</source>
        <translation>场景已更新</translation>
    </message>
    <message>
        <source>Removed provider &apos;%1&apos; and cleared it from %2 scene(s).</source>
        <translation>已移除提供商&quot;%1&quot;，并从 %2 个场景中清除了它的引用。</translation>
    </message>
    <message>
        <source>Add Scene</source>
        <translation>添加场景</translation>
    </message>
    <message>
        <source>Edit Scene</source>
        <translation>编辑场景</translation>
    </message>
    <message>
        <source>autostart</source>
        <translation>自启动</translation>
    </message>
    <message>
        <source>manual</source>
        <translation>手动</translation>
    </message>
    <message>
        <source>Autostart: %1</source>
        <translation>自启动：%1</translation>
    </message>
    <message>
        <source>enabled</source>
        <translation>已启用</translation>
    </message>
    <message>
        <source>disabled</source>
        <translation>已禁用</translation>
    </message>
    <message>
        <source>ID:</source>
        <translation>ID：</translation>
    </message>
    <message>
        <source>Label:</source>
        <translation>标签：</translation>
    </message>
    <message>
        <source>Prompt:</source>
        <translation>提示词：</translation>
    </message>
    <message>
        <source>Provider:</source>
        <translation>提供者：</translation>
    </message>
    <message>
        <source>Model:</source>
        <translation>模型：</translation>
    </message>
    <message>
        <source>Context Lines:</source>
        <translation>上下文行数：</translation>
    </message>
    <message>
        <source>Max LLM Candidates:</source>
        <translation>LLM 最大候选数：</translation>
    </message>
    <message>
        <source>Timeout (ms):</source>
        <translation>超时（毫秒）：</translation>
    </message>
    <message>
        <source>Are you sure you want to remove scene &apos;%1&apos;?</source>
        <translation>确定要移除场景&quot;%1&quot;吗？</translation>
    </message>
    <message>
        <source>LLM provider &apos;%1&apos; already exists.</source>
        <translation>LLM 提供者 &apos;%1&apos; 已存在。</translation>
    </message>
    <message>
        <source>Failed to save config.</source>
        <translation>保存配置失败。</translation>
    </message>
    <message>
        <source>Test</source>
        <translation>测试</translation>
    </message>
    <message>
        <source>Testing...</source>
        <translation>测试中...</translation>
    </message>
    <message>
        <source>Provider &apos;%1&apos; not found.</source>
        <translation>未找到提供商&quot;%1&quot;。</translation>
    </message>
    <message>
        <source>Provider base_url is empty.</source>
        <translation>提供商 base_url 为空。</translation>
    </message>
    <message>
        <source>Test Failed</source>
        <translation>测试失败</translation>
    </message>
    <message>
        <source>Connection to &apos;%1&apos; failed:
%2</source>
        <translation>连接&quot;%1&quot;失败：
%2</translation>
    </message>
    <message>
        <source>Invalid response from &apos;%1&apos;.</source>
        <translation>&quot;%1&quot;返回了无效的响应。</translation>
    </message>
    <message>
        <source>Test Succeeded</source>
        <translation>测试成功</translation>
    </message>
    <message>
        <source>Connected to &apos;%1&apos;.
Found %2 model(s).</source>
        <translation>已连接&quot;%1&quot;。
找到 %2 个模型。</translation>
    </message>
    <message>
        <source>Extra body (JSON):</source>
        <translation>附加请求体 (JSON)：</translation>
    </message>
    <message>
        <source>Optional JSON object merged into each request body, e.g. {&quot;enable_thinking&quot;: false}</source>
        <translation>可选的 JSON 对象，将合并进每次请求体，例如 {&quot;enable_thinking&quot;: false}</translation>
    </message>
    <message>
        <source>Extra body must be a JSON object (e.g. {&quot;enable_thinking&quot;: false}).</source>
        <translation>附加请求体必须是 JSON 对象（例如 {&quot;enable_thinking&quot;: false}）。</translation>
    </message>
    <message>
        <source>Invalid extra_body JSON: %1</source>
        <translation>extra_body JSON 无效：%1</translation>
    </message>
    <message>
        <source>Are you sure you want to remove LLM adapter &apos;%1&apos;?</source>
        <translation>确定要移除 LLM 适配器“%1”吗？</translation>
    </message>
</context>
<context>
    <name>vinput::gui::ResourcePage</name>
    <message>
        <source>&lt;b&gt;Installed Models&lt;/b&gt;</source>
        <translation>&lt;b&gt;已安装模型&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Filter installed models...</source>
        <translation>筛选已安装模型...</translation>
    </message>
    <message>
        <source>Name</source>
        <translation>名称</translation>
    </message>
    <message>
        <source>Type</source>
        <translation>类型</translation>
    </message>
    <message>
        <source>Language</source>
        <translation>语言</translation>
    </message>
    <message>
        <source>Size</source>
        <translation>大小</translation>
    </message>
    <message>
        <source>Hotwords</source>
        <translation>热词</translation>
    </message>
    <message>
        <source>Status</source>
        <translation>状态</translation>
    </message>
    <message>
        <source>Use</source>
        <translation>使用</translation>
    </message>
    <message>
        <source>Remove</source>
        <translation>移除</translation>
    </message>
    <message>
        <source>Refresh</source>
        <translation>刷新</translation>
    </message>
    <message>
        <source>&lt;b&gt;Available Models&lt;/b&gt;</source>
        <translation>&lt;b&gt;可用模型&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Filter available models...</source>
        <translation>筛选可用模型...</translation>
    </message>
    <message>
        <source>Title</source>
        <translation>标题</translation>
    </message>
    <message>
        <source>Description</source>
        <translation>描述</translation>
    </message>
    <message>
        <source>Download</source>
        <translation>下载</translation>
    </message>
    <message>
        <source>&lt;b&gt;Available ASR Providers&lt;/b&gt;</source>
        <translation>&lt;b&gt;可用 ASR 提供商&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Filter available ASR providers...</source>
        <translation>筛选可用 ASR 提供商...</translation>
    </message>
    <message>
        <source>Mode</source>
        <translation>模式</translation>
    </message>
    <message>
        <source>Install</source>
        <translation>安装</translation>
    </message>
    <message>
        <source>&lt;b&gt;Available LLM Adapters&lt;/b&gt;</source>
        <translation>&lt;b&gt;可用 LLM 适配器&lt;/b&gt;</translation>
    </message>
    <message>
        <source>Filter available LLM adapters...</source>
        <translation>筛选可用 LLM 适配器...</translation>
    </message>
    <message>
        <source>Models</source>
        <translation>模型</translation>
    </message>
    <message>
        <source>ASR Providers</source>
        <translation>ASR 提供商</translation>
    </message>
    <message>
        <source>LLM Adapters</source>
        <translation>LLM 适配器</translation>
    </message>
    <message>
        <source>yes</source>
        <translation>是</translation>
    </message>
    <message>
        <source>no</source>
        <translation>否</translation>
    </message>
    <message>
        <source>installed</source>
        <translation>已安装</translation>
    </message>
    <message>
        <source>active</source>
        <translation>激活</translation>
    </message>
    <message>
        <source>broken</source>
        <translation>损坏</translation>
    </message>
    <message>
        <source>available</source>
        <translation>可下载</translation>
    </message>
    <message>
        <source>stream</source>
        <translation>流式</translation>
    </message>
    <message>
        <source>non-stream</source>
        <translation>非流式</translation>
    </message>
    <message>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <source>Selected model &apos;%1&apos; saved as the preferred local ASR model. Backend reload is in progress.</source>
        <translation>已将所选模型&quot;%1&quot;保存为首选本地 ASR 模型。后端正在重新加载。</translation>
    </message>
    <message>
        <source>Confirm</source>
        <translation>确认</translation>
    </message>
    <message>
        <source>Are you sure you want to remove model &apos;%1&apos;?</source>
        <translation>确定要移除模型&quot;%1&quot;吗？</translation>
    </message>
    <message>
        <source>Fetching remote registry...</source>
        <translation>正在获取远程注册表...</translation>
    </message>
    <message>
        <source>Models fetch error: %1</source>
        <translation>获取模型出错：%1</translation>
    </message>
    <message>
        <source>Providers fetch error: %1</source>
        <translation>获取提供者出错：%1</translation>
    </message>
    <message>
        <source>Adapters fetch error: %1</source>
        <translation>获取适配器出错：%1</translation>
    </message>
    <message>
        <source>Registry fetch completed.</source>
        <translation>注册表获取完成。</translation>
    </message>
    <message>
        <source>Registry warning: %1</source>
        <translation>注册表警告：%1</translation>
    </message>
    <message>
        <source>I18n cache reload error: %1</source>
        <translation>i18n 缓存重载错误：%1</translation>
    </message>
    <message>
        <source>Failed to save config.</source>
        <translation>保存配置失败。</translation>
    </message>
    <message>
        <source>Warning</source>
        <translation>警告</translation>
    </message>
    <message>
        <source>Config saved, but failed to reload ASR backend: %1</source>
        <translation>配置已保存，但重新加载 ASR 后端失败：%1</translation>
    </message>
    <message>
        <source>Are you sure you want to remove ASR provider &apos;%1&apos;?</source>
        <translation>确定要移除 ASR 提供商“%1”吗？</translation>
    </message>
    <message>
        <source>Are you sure you want to remove LLM adapter &apos;%1&apos;?</source>
        <translation>确定要移除 LLM 适配器“%1”吗？</translation>
    </message>
    <message>
        <source>Removed ASR provider %1.</source>
        <translation>已移除 ASR 提供商 %1。</translation>
    </message>
    <message>
        <source>Removed LLM adapter %1.</source>
        <translation>已移除 LLM 适配器 %1。</translation>
    </message>
    <message>
        <source>The local ASR provider cannot be removed.</source>
        <translation>本地 ASR 提供商不能被移除。</translation>
    </message>
    <message>
        <source>Removed %1.</source>
        <translation>已移除 %1。</translation>
    </message>
    <message>
        <source>Downloading... %1% at %2</source>
        <translation>正在下载... %1% 速度 %2</translation>
    </message>
    <message>
        <source>Preparing download...</source>
        <translation>正在准备下载...</translation>
    </message>
    <message>
        <source>Download Error</source>
        <translation>下载错误</translation>
    </message>
    <message>
        <source>Starting download for %1...</source>
        <translation>开始下载 %1...</translation>
    </message>
    <message>
        <source>Installing provider %1...</source>
        <translation>正在安装提供者 %1...</translation>
    </message>
    <message>
        <source>Installing adapter %1...</source>
        <translation>正在安装适配器 %1...</translation>
    </message>
</context>
</TS>
