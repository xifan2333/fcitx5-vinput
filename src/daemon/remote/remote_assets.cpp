#include "daemon/remote/remote_assets.h"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "common/utils/path_utils.h"

namespace vinput::daemon::remote {

namespace {

constexpr std::string_view kFallbackIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>VInput Remote</title>
  <style>
    :root {
      color-scheme: light dark;
      --bg: #111418;
      --panel: #1a2027;
      --panel-2: #202832;
      --text: #f2f5f8;
      --muted: #9aa7b5;
      --border: #34404c;
      --accent: #1f9d87;
      --danger: #e15a5a;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    main {
      width: min(760px, calc(100vw - 32px));
      margin: 0 auto;
      padding: 40px 0;
    }
    h1 {
      margin: 0 0 6px;
      font-size: 28px;
      line-height: 1.2;
      letter-spacing: 0;
    }
    .sub {
      color: var(--muted);
      font-size: 14px;
      margin-bottom: 24px;
    }
    .status {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
      margin-bottom: 16px;
    }
    .tile {
      border: 1px solid var(--border);
      background: var(--panel);
      border-radius: 8px;
      padding: 14px;
      min-height: 76px;
    }
    .label {
      color: var(--muted);
      font-size: 13px;
      margin-bottom: 8px;
    }
    .value {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 16px;
      font-weight: 600;
    }
    .dot {
      width: 9px;
      height: 9px;
      border-radius: 999px;
      background: var(--danger);
      flex: 0 0 auto;
    }
    .dot.on { background: var(--accent); }
    textarea {
      width: 100%;
      min-height: 45vh;
      resize: vertical;
      border: 1px solid var(--border);
      border-radius: 8px;
      background: var(--panel-2);
      color: var(--text);
      padding: 16px;
      font: 18px/1.6 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      outline: none;
    }
    textarea:focus { border-color: var(--accent); }
    textarea:disabled { opacity: 0.58; cursor: not-allowed; }
    .bar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-top: 12px;
      color: var(--muted);
      font-size: 13px;
    }
    button {
      border: 1px solid var(--border);
      border-radius: 8px;
      background: var(--panel);
      color: var(--text);
      padding: 9px 13px;
      font: inherit;
      cursor: pointer;
    }
    button:disabled { opacity: 0.5; cursor: not-allowed; }
    @media (max-width: 560px) {
      main { width: min(100vw - 24px, 760px); padding: 24px 0; }
      .status { grid-template-columns: 1fr; }
      textarea { min-height: 52vh; font-size: 17px; }
    }
  </style>
</head>
<body>
  <main>
    <h1>VInput Remote</h1>
    <div class="sub">Local network text input</div>
    <section class="status">
      <div class="tile">
        <div class="label">Phone input</div>
        <div class="value"><span id="input-dot" class="dot"></span><span id="input-status">connecting</span></div>
      </div>
      <div class="tile">
        <div class="label">VInput session</div>
        <div class="value"><span id="output-dot" class="dot"></span><span id="output-status">ready</span></div>
      </div>
    </section>
    <textarea id="editor" spellcheck="false" enterkeyhint="send" disabled placeholder="Type or use mobile voice input here..."></textarea>
    <div class="bar">
      <span><span id="count">0</span> chars</span>
      <button id="finalize" type="button" disabled>Send</button>
    </div>
  </main>
  <script>
    const editor = document.getElementById('editor')
    const finalize = document.getElementById('finalize')
    const count = document.getElementById('count')
    const inputStatus = document.getElementById('input-status')
    const outputStatus = document.getElementById('output-status')
    const inputDot = document.getElementById('input-dot')
    const outputDot = document.getElementById('output-dot')
    let ws = null
    let composing = false
    let outputConnected = false
    let authenticated = false
    let apiKey = ''
    let terminalError = ''

    function readApiKey() {
      const params = new URLSearchParams(location.hash.replace(/^#/, ''))
      const hashKey = params.get('key') || ''
      if (hashKey) {
        localStorage.setItem('vinput_remote_api_key', hashKey)
        history.replaceState(null, '', location.pathname)
        return hashKey
      }
      const stored = localStorage.getItem('vinput_remote_api_key') || ''
      if (stored) return stored
      const entered = window.prompt('API key') || ''
      if (entered) localStorage.setItem('vinput_remote_api_key', entered)
      return entered
    }

    function setInput(state) {
      if (terminalError && state === 'disconnected') {
        return
      }
      inputStatus.textContent = state
      inputDot.classList.toggle('on', state === 'connected')
      updateEnabled()
    }
    function setOutput(state) {
      outputConnected = state === 'connected'
      outputStatus.textContent = state
      outputDot.classList.toggle('on', outputConnected)
    }
    function updateEnabled() {
      const enabled = ws && ws.readyState === WebSocket.OPEN && authenticated
      editor.disabled = !enabled
      finalize.disabled = !enabled
      if (enabled && document.activeElement !== editor) {
        editor.focus()
      }
    }
    function send(payload) {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(payload))
      }
    }
    function commitAndClear() {
      const text = editor.value
      if (text.length === 0) {
        return
      }
      send({ type: 'text_update', text: text })
      send({ type: 'finalize' })
      editor.value = ''
      count.textContent = '0'
      editor.focus()
    }
    const messageHandlers = {
      auth_ok: () => {
        authenticated = true
        terminalError = ''
        setInput('connected')
        updateEnabled()
      },
      init: msg => setOutput(msg.output_status || 'ready'),
      output_connected: () => setOutput('connected'),
      output_disconnected: () => setOutput('ready'),
      committed: () => {
        editor.value = ''
        count.textContent = '0'
      },
      busy: () => {
        authenticated = false
        terminalError = 'busy'
        setInput('busy (occupied, retrying...)')
        updateEnabled()
      },
      error: msg => {
        authenticated = false
        terminalError = 'error'
        localStorage.removeItem('vinput_remote_api_key')
        apiKey = ''
        setInput(msg && msg.message ? `failed: ${msg.message}` : 'failed')
        updateEnabled()
      }
    }

    function connect() {
      if (terminalError === 'error') {
        return
      }
      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
      ws = new WebSocket(proto + '//' + location.host + '/ws')
      if (!terminalError) {
        setInput('connecting')
      }
      ws.onopen = () => {
        apiKey = apiKey || readApiKey()
        if (!apiKey) {
          terminalError = 'error'
          setInput('failed: API key required')
          ws.close()
          return
        }
        send({ type: 'auth', api_key: apiKey })
      }
      ws.onmessage = event => {
        try {
          const msg = JSON.parse(event.data)
          messageHandlers[msg?.type]?.(msg)
        } catch {
          // Ignore invalid JSON payloads
        }
      }
      ws.onclose = () => {
        authenticated = false
        if (!terminalError) {
          setInput('disconnected')
        }
        setOutput('ready')
        updateEnabled()
        if (terminalError !== 'error') {
          setTimeout(connect, terminalError === 'busy' ? 2000 : 1000)
        }
      }
      ws.onerror = () => ws.close()
    }
    editor.addEventListener('compositionstart', () => { composing = true })
    editor.addEventListener('compositionend', () => {
      composing = false
      count.textContent = String(editor.value.length)
      send({ type: 'text_update', text: editor.value })
    })
    editor.addEventListener('input', () => {
      count.textContent = String(editor.value.length)
      if (!composing) send({ type: 'text_update', text: editor.value })
    })
    editor.addEventListener('keydown', e => {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault()
        commitAndClear()
      }
    })
    finalize.addEventListener('click', () => commitAndClear())
    connect()
  </script>
</body>
</html>
)HTML";

constexpr std::string_view kFallbackFaviconSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="12" fill="#1f9d87"/><path d="M32 12c-5 0-9 4-9 9v16c0 5 4 9 9 9s9-4 9-9V21c0-5-4-9-9-9z" fill="#fff"/><path d="M17 33c0 8 7 15 15 15s15-7 15-15" fill="none" stroke="#fff" stroke-width="5" stroke-linecap="round"/><path d="M32 48v8" stroke="#fff" stroke-width="5" stroke-linecap="round"/></svg>)SVG";

} // namespace

std::string LoadRemoteIndexHtml() {
  const auto path = vinput::path::RemoteWebStaticDir() / "index.html";
  std::ifstream file(path);
  if (file.is_open()) {
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();
    if (!content.empty()) {
      return content;
    }
  }
  return std::string(kFallbackIndexHtml);
}

std::string LoadRemoteFaviconSvg() {
  const auto path = vinput::path::RemoteWebStaticDir() / "favicon.svg";
  std::ifstream file(path);
  if (file.is_open()) {
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();
    if (!content.empty()) {
      return content;
    }
  }
  return std::string(kFallbackFaviconSvg);
}

} // namespace vinput::daemon::remote
