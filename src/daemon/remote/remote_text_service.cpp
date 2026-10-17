#include "daemon/remote/remote_text_service.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <ifaddrs.h>
#include <map>
#include <net/if.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "common/config/core_config.h"
#include "common/utils/interpolation.h"
#include "common/utils/string_utils.h"

namespace vinput::daemon::remote {

namespace {

using json = nlohmann::json;

constexpr std::string_view kRemoteProviderId = "provider.vinput.remote.streaming";
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxHttpHeaderBytes = 65536;
constexpr std::size_t kMaxWebSocketPayloadBytes = 2 * 1024 * 1024;

const char kIndexHtml[] = R"HTML(<!doctype html>
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
        <div class="value"><span id="output-dot" class="dot"></span><span id="output-status">disconnected</span></div>
      </div>
    </section>
    <textarea id="editor" spellcheck="false" disabled placeholder="Connect VInput, then type here."></textarea>
    <div class="bar">
      <span><span id="count">0</span> chars</span>
      <button id="finalize" type="button" disabled>Send now</button>
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
    let apiKey = ''

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
      inputStatus.textContent = state
      inputDot.classList.toggle('on', state === 'connected')
      updateEnabled()
    }
    function setOutput(state) {
      outputConnected = state === 'connected'
      outputStatus.textContent = state
      outputDot.classList.toggle('on', outputConnected)
      if (!outputConnected) {
        editor.value = ''
        count.textContent = '0'
      }
      updateEnabled()
    }
    function updateEnabled() {
      const enabled = ws && ws.readyState === WebSocket.OPEN && outputConnected
      editor.disabled = !enabled
      finalize.disabled = !enabled
      if (enabled) editor.focus()
    }
    function send(payload) {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(payload))
      }
    }
    function connect() {
      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
      ws = new WebSocket(proto + '//' + location.host + '/ws')
      setInput('connecting')
      ws.onopen = () => {
        apiKey = apiKey || readApiKey()
        send({ type: 'auth', api_key: apiKey })
      }
      ws.onmessage = event => {
        let msg
        try { msg = JSON.parse(event.data) } catch { return }
        if (msg.type === 'auth_ok') setInput('connected')
        if (msg.type === 'init') setOutput(msg.output_status || 'disconnected')
        if (msg.type === 'output_connected') setOutput('connected')
        if (msg.type === 'output_disconnected') setOutput('disconnected')
        if (msg.type === 'error') {
          localStorage.removeItem('vinput_remote_api_key')
          apiKey = ''
          setInput('failed')
        }
      }
      ws.onclose = () => {
        setInput('disconnected')
        setOutput('disconnected')
        setTimeout(connect, 1000)
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
    finalize.addEventListener('click', () => send({ type: 'finalize' }))
    connect()
  </script>
</body>
</html>
)HTML";

const char kFaviconSvg[] =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="12" fill="#1f9d87"/><path d="M32 12c-5 0-9 4-9 9v16c0 5 4 9 9 9s9-4 9-9V21c0-5-4-9-9-9z" fill="#fff"/><path d="M17 33c0 8 7 15 15 15s15-7 15-15" fill="none" stroke="#fff" stroke-width="5" stroke-linecap="round"/><path d="M32 48v8" stroke="#fff" stroke-width="5" stroke-linecap="round"/></svg>)SVG";

std::optional<int> ParsePositiveInt(std::string_view value) {
  const std::string trimmed = vinput::str::TrimAsciiWhitespace(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  const char* begin = trimmed.data();
  const char* end = begin + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed <= 0) {
    return std::nullopt;
  }
  return parsed;
}

std::string EnvValue(const std::map<std::string, std::string>& env, std::string_view key) {
  const auto it = env.find(std::string(key));
  if (it == env.end()) {
    return {};
  }
  return vinput::str::TrimAsciiWhitespace(it->second);
}

std::optional<int> ParsePortFromWsUrl(std::string_view url) {
  const std::string text = vinput::str::TrimAsciiWhitespace(url);
  const std::size_t scheme = text.find("://");
  if (scheme == std::string::npos) {
    return std::nullopt;
  }
  std::size_t authority_begin = scheme + 3;
  std::size_t authority_end = text.find('/', authority_begin);
  if (authority_end == std::string::npos) {
    authority_end = text.size();
  }
  std::string_view authority(text.data() + authority_begin, authority_end - authority_begin);
  if (authority.empty()) {
    return std::nullopt;
  }
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos || close + 1 >= authority.size() ||
        authority[close + 1] != ':') {
      return std::nullopt;
    }
    return ParsePositiveInt(authority.substr(close + 2));
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos || colon + 1 >= authority.size()) {
    return std::nullopt;
  }
  return ParsePositiveInt(authority.substr(colon + 1));
}

std::optional<std::string> Sha1Base64(std::string_view text) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return std::nullopt;
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  const bool ok = EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, text.data(), text.size()) == 1 &&
                  EVP_DigestFinal_ex(ctx, digest, &digest_size) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok) {
    return std::nullopt;
  }
  return vinput::str::Base64Encode(digest, digest_size);
}

bool WriteAll(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool SendHttpResponse(int fd, std::string_view status, std::string_view content_type,
                      std::string_view body) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  return WriteAll(fd, out.str());
}

bool ReadHttpRequest(int fd, std::string* request) {
  request->clear();
  char buffer[4096];
  while (request->find("\r\n\r\n") == std::string::npos) {
    const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
      request->append(buffer, static_cast<std::size_t>(n));
      if (request->size() > kMaxHttpHeaderBytes) {
        return false;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

struct HttpRequestInfo {
  std::string method;
  std::string target;
  std::string path;
  std::map<std::string, std::string> headers;
};

std::optional<HttpRequestInfo> ParseHttpRequest(const std::string& request) {
  std::istringstream stream(request.substr(0, request.find("\r\n\r\n")));
  std::string line;
  if (!std::getline(stream, line)) {
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  HttpRequestInfo info;
  std::istringstream request_line(line);
  request_line >> info.method >> info.target;
  if (info.method.empty() || info.target.empty()) {
    return std::nullopt;
  }
  info.path = info.target.substr(0, info.target.find('?'));
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto header = vinput::str::SplitPair(line, ':', /*trim_whitespace=*/true);
    if (!header) {
      continue;
    }
    std::string name = vinput::str::ToLowerAscii(header->first);
    if (!name.empty()) {
      info.headers[std::move(name)] = std::string(header->second);
    }
  }
  return info;
}

std::string BearerToken(const std::map<std::string, std::string>& headers) {
  const auto it = headers.find("authorization");
  if (it == headers.end()) {
    return {};
  }
  std::string_view value = vinput::str::TrimAsciiWhitespaceView(it->second);
  if (!vinput::str::ConsumePrefix(&value, "Bearer ")) {
    return {};
  }
  return vinput::str::TrimAsciiWhitespace(value);
}

bool SendWebSocketHandshake(int fd, const HttpRequestInfo& request) {
  const auto key_it = request.headers.find("sec-websocket-key");
  if (key_it == request.headers.end() || key_it->second.empty()) {
    SendHttpResponse(fd, "400 Bad Request", "text/plain; charset=utf-8",
                     "Missing Sec-WebSocket-Key.\n");
    return false;
  }
  const std::string accept_source =
      key_it->second + std::string(kWebSocketGuid.data(), kWebSocketGuid.size());
  const auto accept = Sha1Base64(accept_source);
  if (!accept) {
    SendHttpResponse(fd, "500 Internal Server Error", "text/plain; charset=utf-8",
                     "Failed to create WebSocket handshake.\n");
    return false;
  }
  std::ostringstream out;
  out << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << *accept << "\r\n\r\n";
  return WriteAll(fd, out.str());
}

bool ReadExact(int fd, std::string* out, std::size_t size) {
  out->clear();
  out->resize(size);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t n = recv(fd, out->data() + offset, size - offset, 0);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool ReadUint16(int fd, std::uint16_t* value) {
  std::string bytes;
  if (!ReadExact(fd, &bytes, 2)) {
    return false;
  }
  *value = (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0])) << 8) |
           static_cast<unsigned char>(bytes[1]);
  return true;
}

bool ReadUint64(int fd, std::uint64_t* value) {
  std::string bytes;
  if (!ReadExact(fd, &bytes, 8)) {
    return false;
  }
  std::uint64_t out = 0;
  for (unsigned char ch : bytes) {
    out = (out << 8) | ch;
  }
  *value = out;
  return true;
}

struct WebSocketFrame {
  bool fin = false;
  std::uint8_t opcode = 0;
  std::string payload;
};

bool SendWebSocketFrame(int fd, std::uint8_t opcode, std::string_view payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xffff) {
    frame.push_back(126);
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
    frame.push_back(static_cast<char>(payload.size() & 0xff));
  } else {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((payload.size() >> shift) & 0xff));
    }
  }
  frame.append(payload);
  return WriteAll(fd, frame);
}

bool SendWebSocketJson(int fd, const json& payload) {
  return SendWebSocketFrame(fd, 0x1, payload.dump());
}

bool ReadWebSocketFrame(int fd, WebSocketFrame* frame) {
  std::string header;
  if (!ReadExact(fd, &header, 2)) {
    return false;
  }
  const auto first = static_cast<unsigned char>(header[0]);
  const auto second = static_cast<unsigned char>(header[1]);
  frame->fin = (first & 0x80) != 0;
  frame->opcode = first & 0x0f;
  const bool masked = (second & 0x80) != 0;
  std::uint64_t length = second & 0x7f;
  if (length == 126) {
    std::uint16_t short_length = 0;
    if (!ReadUint16(fd, &short_length)) {
      return false;
    }
    length = short_length;
  } else if (length == 127) {
    if (!ReadUint64(fd, &length)) {
      return false;
    }
  }
  if (length > kMaxWebSocketPayloadBytes) {
    return false;
  }
  std::string mask_key;
  if (masked && !ReadExact(fd, &mask_key, 4)) {
    return false;
  }
  std::string payload;
  if (length > 0 && !ReadExact(fd, &payload, static_cast<std::size_t>(length))) {
    return false;
  }
  if (masked) {
    for (std::size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<char>(payload[i] ^ mask_key[i % 4]);
    }
  }
  frame->payload = std::move(payload);
  return true;
}

bool ReadWebSocketTextMessage(int fd, std::mutex* write_mutex, std::string* message) {
  message->clear();
  std::uint8_t current_opcode = 0;
  while (true) {
    WebSocketFrame frame;
    if (!ReadWebSocketFrame(fd, &frame)) {
      return false;
    }
    if (frame.opcode == 0x8) {
      return false;
    }
    if (frame.opcode == 0x9) {
      std::lock_guard<std::mutex> lock(*write_mutex);
      SendWebSocketFrame(fd, 0xA, frame.payload);
      continue;
    }
    if (frame.opcode == 0xA) {
      continue;
    }
    if (frame.opcode == 0x1) {
      current_opcode = frame.opcode;
      *message = std::move(frame.payload);
    } else if (frame.opcode == 0x0 && current_opcode == 0x1) {
      message->append(frame.payload);
    } else {
      continue;
    }
    if (frame.fin) {
      return true;
    }
  }
}

std::string NewEventId(std::string_view prefix) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return std::string(prefix) + "_" + std::to_string(ns);
}

bool IsLoopbackAddress(const sockaddr_storage& addr) {
  if (addr.ss_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
    const std::uint32_t host = ntohl(in->sin_addr.s_addr);
    return (host & 0xff000000u) == 0x7f000000u;
  }
  if (addr.ss_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
  }
  return false;
}

bool PeerIsLoopback(int fd) {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return false;
  }
  return IsLoopbackAddress(addr);
}

void ShutdownSocket(int fd) {
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
  }
}

void CloseSocket(int fd) {
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
}

} // namespace

RemoteTextService::~RemoteTextService() {
  Shutdown();
}

bool RemoteTextService::ExtractSettings(const CoreConfig& config, Settings* settings,
                                        bool* should_run, std::string* error) const {
  if (!settings) {
    if (error) {
      *error = "settings output is null";
    }
    return false;
  }
  if (should_run) {
    *should_run = false;
  }

  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  const auto* command = provider ? std::get_if<CommandAsrProvider>(provider) : nullptr;
  if (!command) {
    if (error) {
      error->clear();
    }
    return true;
  }

  const bool is_remote = command->id == kRemoteProviderId;
  if (!is_remote) {
    if (error) {
      error->clear();
    }
    return true;
  }

  Settings out;
  const std::string port_text = EnvValue(command->env, "VINPUT_ASR_PORT");
  if (!port_text.empty()) {
    const auto port = ParsePositiveInt(port_text);
    if (!port || *port > 65535) {
      if (error) {
        *error = "VINPUT_ASR_PORT must be between 1 and 65535.";
      }
      return false;
    }
    out.port = *port;
  } else if (const auto port = ParsePortFromWsUrl(EnvValue(command->env, "VINPUT_ASR_URL"))) {
    if (*port <= 65535) {
      out.port = *port;
    }
  }

  const std::string debounce_text = EnvValue(command->env, "VINPUT_ASR_DEBOUNCE_MS");
  if (!debounce_text.empty()) {
    const auto debounce = ParsePositiveInt(debounce_text);
    if (!debounce) {
      if (error) {
        *error = "VINPUT_ASR_DEBOUNCE_MS must be a positive integer.";
      }
      return false;
    }
    out.debounce_ms = *debounce;
  }

  out.api_key = vinput::interpolate::InterpolateEnv(EnvValue(command->env, "VINPUT_ASR_API_KEY"));
  if (out.api_key.empty()) {
    if (error) {
      *error = "Remote ASR provider requires VINPUT_ASR_API_KEY.";
    }
    return false;
  }

  *settings = std::move(out);
  if (should_run) {
    *should_run = true;
  }
  if (error) {
    error->clear();
  }
  return true;
}

bool RemoteTextService::OpenListenSocket(const Settings& settings, std::string* error) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error) {
      *error = std::string("failed to create remote ASR listen socket: ") + std::strerror(errno);
    }
    return false;
  }

  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<std::uint16_t>(settings.port));

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 16) != 0) {
    const int saved_errno = errno;
    close(fd);
    if (error) {
      *error = "failed to bind remote ASR service on 0.0.0.0:" + std::to_string(settings.port) +
               ": " + std::strerror(saved_errno);
    }
    return false;
  }

  listen_fd_ = fd;
  if (error) {
    error->clear();
  }
  return true;
}

bool RemoteTextService::Synchronize(const CoreConfig& config, std::string* error) {
  Settings next_settings;
  bool should_run = false;
  if (!ExtractSettings(config, &next_settings, &should_run, error)) {
    Shutdown();
    return false;
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!should_run) {
    if (service_started_) {
      Shutdown();
    }
    if (error) {
      error->clear();
    }
    return true;
  }

  const bool same_settings = service_started_ && settings_.port == next_settings.port &&
                             settings_.debounce_ms == next_settings.debounce_ms &&
                             settings_.api_key == next_settings.api_key;
  if (same_settings) {
    if (error) {
      error->clear();
    }
    return true;
  }

  if (service_started_) {
    Shutdown();
  }
  return Start(next_settings, error);
}

std::vector<std::string> RemoteTextService::ListEndpoints() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!service_started_) {
    return {};
  }

  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) {
    return {};
  }

  std::vector<std::string> endpoints;
  for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    const auto* sa = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
    char buf[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)) == nullptr) {
      continue;
    }
    endpoints.emplace_back(std::string("http://") + buf + ":" + std::to_string(settings_.port));
  }
  freeifaddrs(ifaddr);

  std::sort(endpoints.begin(), endpoints.end());
  endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
  return endpoints;
}

bool RemoteTextService::Start(const Settings& settings, std::string* error) {
  settings_ = settings;
  if (!OpenListenSocket(settings_, error)) {
    return false;
  }

  running_.store(true, std::memory_order_release);
  debounce_pending_ = false;
  debounce_stop_ = false;
  try {
    debounce_thread_ = std::thread([this]() { DebounceLoop(); });
    accept_thread_ = std::thread([this]() { AcceptLoop(); });
  } catch (const std::exception& e) {
    running_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(debounce_mutex_);
      debounce_stop_ = true;
    }
    debounce_cv_.notify_all();
    if (listen_fd_ >= 0) {
      ShutdownSocket(listen_fd_);
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (debounce_thread_.joinable()) {
      debounce_thread_.join();
    }
    if (error) {
      *error = std::string("failed to start remote ASR service thread: ") + e.what();
    }
    return false;
  }

  service_started_ = true;
  std::fprintf(stderr, "vinput-daemon: remote ASR service listening on 0.0.0.0:%d\n",
               settings_.port);
  if (error) {
    error->clear();
  }
  return true;
}

void RemoteTextService::Shutdown() {
  const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> lock(debounce_mutex_);
    debounce_stop_ = true;
    debounce_pending_ = false;
  }
  debounce_cv_.notify_all();

  if (listen_fd_ >= 0) {
    ShutdownSocket(listen_fd_);
    close(listen_fd_);
    listen_fd_ = -1;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ShutdownSocket(input_fd_);
    ShutdownSocket(output_fd_);
  }
  {
    std::lock_guard<std::mutex> lock(client_fds_mutex_);
    for (int fd : client_fds_) {
      ShutdownSocket(fd);
    }
  }

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    threads.swap(client_threads_);
  }
  for (auto& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (debounce_thread_.joinable()) {
    debounce_thread_.join();
  }

  if (was_running) {
    ClearText();
  }
  service_started_ = false;
}

void RemoteTextService::AcceptLoop() {
  while (running_.load(std::memory_order_acquire)) {
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (running_.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "vinput-daemon: remote ASR accept failed: %s\n", std::strerror(errno));
      }
      break;
    }

    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    client_threads_.emplace_back([this, fd]() { HandleClient(fd); });
  }
}

void RemoteTextService::HandleClient(int fd) {
  {
    std::lock_guard<std::mutex> lock(client_fds_mutex_);
    client_fds_.push_back(fd);
  }

  std::string request;
  if (ReadHttpRequest(fd, &request)) {
    HandleHttpRequest(fd, request);
  }

  {
    std::lock_guard<std::mutex> lock(client_fds_mutex_);
    auto it = std::remove(client_fds_.begin(), client_fds_.end(), fd);
    client_fds_.erase(it, client_fds_.end());
  }
  CloseSocket(fd);
}

void RemoteTextService::HandleHttpRequest(int fd, const std::string& request) {
  const auto parsed = ParseHttpRequest(request);
  if (!parsed) {
    SendHttpResponse(fd, "400 Bad Request", "text/plain; charset=utf-8", "Invalid request.\n");
    return;
  }

  const auto& info = *parsed;
  if (info.path == "/ws" || info.path == "/v1/realtime") {
    const bool peer_is_loopback = PeerIsLoopback(fd);
    if (info.path == "/v1/realtime" && !peer_is_loopback) {
      SendHttpResponse(fd, "403 Forbidden", "text/plain; charset=utf-8",
                       "Realtime endpoint is local-only.\n");
      return;
    }
    if (info.path == "/v1/realtime" && !ValidateApiKey(BearerToken(info.headers))) {
      SendHttpResponse(fd, "401 Unauthorized", "text/plain; charset=utf-8", "Unauthorized.\n");
      return;
    }
    if (!SendWebSocketHandshake(fd, info)) {
      return;
    }
    if (info.path == "/ws") {
      HandleInput(fd);
    } else {
      HandleRealtime(fd, peer_is_loopback);
    }
    return;
  }

  if (info.method != "GET") {
    SendHttpResponse(fd, "405 Method Not Allowed", "text/plain; charset=utf-8",
                     "Method not allowed.\n");
    return;
  }
  if (info.path == "/health") {
    SendHttpResponse(fd, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}\n");
    return;
  }
  if (info.path == "/favicon.svg") {
    SendHttpResponse(fd, "200 OK", "image/svg+xml", kFaviconSvg);
    return;
  }
  SendHttpResponse(fd, "200 OK", "text/html; charset=utf-8", kIndexHtml);
}

bool RemoteTextService::TrySetOutput(int fd) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (output_fd_ >= 0) {
    return false;
  }
  output_fd_ = fd;
  return true;
}

bool RemoteTextService::TrySetInput(int fd) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (input_fd_ >= 0) {
    return false;
  }
  input_fd_ = fd;
  return true;
}

void RemoteTextService::RemoveOutput(int fd) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (output_fd_ == fd) {
      output_fd_ = -1;
    }
  }
  CancelDebounce();
  ClearText();
  NotifyInput("output_disconnected");
}

void RemoteTextService::RemoveInput(int fd) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (input_fd_ == fd) {
      input_fd_ = -1;
    }
  }
  CancelDebounce();
}

void RemoteTextService::NotifyInput(std::string_view type) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    fd = input_fd_;
  }
  if (fd < 0) {
    return;
  }
  std::lock_guard<std::mutex> write_lock(input_write_mutex_);
  SendWebSocketJson(fd, json{{"type", type}});
}

void RemoteTextService::HandleRealtime(int fd, bool peer_is_loopback) {
  if (!peer_is_loopback) {
    return;
  }
  if (!TrySetOutput(fd)) {
    std::lock_guard<std::mutex> write_lock(output_write_mutex_);
    SendWebSocketJson(fd, json{{"event_id", NewEventId("event")},
                               {"type", "error"},
                               {"error", json{{"message", "Output client already connected."}}}});
    return;
  }

  NotifyInput("output_connected");
  while (running_.load(std::memory_order_acquire)) {
    std::string message;
    if (!ReadWebSocketTextMessage(fd, &output_write_mutex_, &message)) {
      break;
    }

    json event;
    try {
      event = json::parse(message);
    } catch (const std::exception&) {
      continue;
    }

    const std::string type = event.value("type", std::string{});
    if (type == "session.update") {
      json session = json::object();
      if (event.contains("session")) {
        session = event["session"];
      }
      std::lock_guard<std::mutex> write_lock(output_write_mutex_);
      SendWebSocketJson(fd, json{{"event_id", NewEventId("event")},
                                 {"type", "session.updated"},
                                 {"session", std::move(session)}});
    } else if (type == "input_audio_buffer.append") {
      // This fake ASR service receives text from /ws and ignores audio frames.
    } else if (type == "input_audio_buffer.commit") {
      if (GetText().empty()) {
        SendEmptyCommit(fd);
      } else {
        CancelDebounce();
        SendFinalResult();
      }
    }
  }

  RemoveOutput(fd);
}

bool RemoteTextService::ValidateApiKey(std::string_view api_key) const {
  const std::string expected = settings_.api_key;
  if (expected.empty() || api_key.size() != expected.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    diff |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(api_key[i]);
  }
  return diff == 0;
}

bool RemoteTextService::AuthenticateInput(int fd) {
  std::string message;
  if (!ReadWebSocketTextMessage(fd, &input_write_mutex_, &message)) {
    return false;
  }

  json event;
  try {
    event = json::parse(message);
  } catch (const std::exception&) {
    std::lock_guard<std::mutex> write_lock(input_write_mutex_);
    SendWebSocketJson(fd, json{{"type", "error"}, {"message", "Invalid JSON."}});
    return false;
  }

  const std::string type = event.value("type", std::string{});
  const std::string api_key = event.value("api_key", std::string{});
  if (type != "auth" || !ValidateApiKey(api_key)) {
    std::lock_guard<std::mutex> write_lock(input_write_mutex_);
    SendWebSocketJson(fd, json{{"type", "error"}, {"message", "Unauthorized."}});
    return false;
  }

  std::lock_guard<std::mutex> write_lock(input_write_mutex_);
  SendWebSocketJson(fd, json{{"type", "auth_ok"}});
  return true;
}

void RemoteTextService::HandleInput(int fd) {
  if (!AuthenticateInput(fd)) {
    return;
  }

  if (!TrySetInput(fd)) {
    std::lock_guard<std::mutex> write_lock(input_write_mutex_);
    SendWebSocketJson(fd, json{{"type", "error"}, {"message", "Input client already connected."}});
    return;
  }

  {
    std::string output_status = "disconnected";
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (output_fd_ >= 0) {
        output_status = "connected";
      }
    }
    std::lock_guard<std::mutex> write_lock(input_write_mutex_);
    SendWebSocketJson(fd, json{{"type", "init"}, {"output_status", output_status}});
  }

  while (running_.load(std::memory_order_acquire)) {
    std::string message;
    if (!ReadWebSocketTextMessage(fd, &input_write_mutex_, &message)) {
      break;
    }

    json event;
    try {
      event = json::parse(message);
    } catch (const std::exception&) {
      continue;
    }

    const std::string type = event.value("type", std::string{});
    if (type == "text_update") {
      SetText(event.value("text", std::string{}));
      ScheduleDebounce();
    } else if (type == "finalize") {
      ForceDebounce();
    }
  }

  RemoveInput(fd);
}

void RemoteTextService::SetText(std::string text) {
  std::lock_guard<std::mutex> lock(text_mutex_);
  current_text_ = std::move(text);
}

std::string RemoteTextService::GetText() const {
  std::lock_guard<std::mutex> lock(text_mutex_);
  return current_text_;
}

void RemoteTextService::ClearText() {
  std::lock_guard<std::mutex> lock(text_mutex_);
  current_text_.clear();
}

void RemoteTextService::ScheduleDebounce() {
  {
    std::lock_guard<std::mutex> lock(debounce_mutex_);
    debounce_pending_ = true;
    debounce_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(settings_.debounce_ms);
  }
  debounce_cv_.notify_all();
}

void RemoteTextService::CancelDebounce() {
  {
    std::lock_guard<std::mutex> lock(debounce_mutex_);
    debounce_pending_ = false;
  }
  debounce_cv_.notify_all();
}

void RemoteTextService::ForceDebounce() {
  CancelDebounce();
  SendFinalResult();
}

void RemoteTextService::DebounceLoop() {
  std::unique_lock<std::mutex> lock(debounce_mutex_);
  while (!debounce_stop_) {
    debounce_cv_.wait(lock, [this]() { return debounce_stop_ || debounce_pending_; });
    while (!debounce_stop_ && debounce_pending_) {
      const auto deadline = debounce_deadline_;
      if (debounce_cv_.wait_until(lock, deadline, [this, deadline]() {
            return debounce_stop_ || !debounce_pending_ || debounce_deadline_ != deadline;
          })) {
        continue;
      }
      debounce_pending_ = false;
      lock.unlock();
      SendFinalResult();
      lock.lock();
    }
  }
}

void RemoteTextService::SendEmptyCommit(int fd) {
  std::lock_guard<std::mutex> write_lock(output_write_mutex_);
  SendWebSocketJson(fd, json{{"event_id", NewEventId("event")},
                             {"type", "input_audio_buffer.committed"},
                             {"item_id", NewEventId("item")}});
}

void RemoteTextService::SendFinalResult() {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    fd = output_fd_;
  }
  if (fd < 0) {
    return;
  }

  const std::string text = GetText();
  if (text.empty()) {
    return;
  }

  const std::string item_id = NewEventId("item");
  {
    std::lock_guard<std::mutex> write_lock(output_write_mutex_);
    SendWebSocketJson(fd, json{{"event_id", NewEventId("event")},
                               {"type", "input_audio_buffer.committed"},
                               {"item_id", item_id}});
    SendWebSocketJson(fd, json{{"event_id", NewEventId("event")},
                               {"type", "conversation.item.input_audio_transcription.delta"},
                               {"item_id", item_id},
                               {"delta", text}});
    SendWebSocketJson(fd, json{{"event_id", NewEventId("event")},
                               {"type", "conversation.item.input_audio_transcription.completed"},
                               {"item_id", item_id},
                               {"transcript", text}});
  }
  ClearText();
}

} // namespace vinput::daemon::remote
