// ds-adapter: DeepSeek Responses local adapter (C++, Win32, no external deps)
// Modes:
//   adapter.exe [port]          - run proxy on 127.0.0.1:<port> (default 8787) -> api.deepseek.com
//   adapter.exe watchdog [port] - start adapter.exe if the port is not listening
//   adapter.exe selftest [port] - verify local listener + upstream reachability (writes selftest.log)
//   adapter.exe test in.json out.json - rewrite JSON file (self test)
// Every rewritten subagent task is also appended to tasks.log (plaintext) so
// the user can see exactly what the parent agent sent to the child.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <process.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

static const wchar_t* kUpstreamHost = L"api.deepseek.com";
static const char* kVersion = "1.2.0";
static int g_port = 8787;

static std::wstring g_exeDir;
static std::wstring g_exePath;
static std::wstring g_logPath;
static std::wstring g_taskLogPath;

static std::string NowIso() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[64];
  sprintf_s(b, "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return b;
}

static void Log(const char* msg) {
  HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string line = NowIso() + " " + msg + "\r\n";
  DWORD written = 0;
  WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
  CloseHandle(h);
}

static void LogTask(const std::string& msg) {
  HANDLE h = CreateFileW(g_taskLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string line = NowIso() + " " + msg + "\r\n";
  DWORD written = 0;
  WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
  CloseHandle(h);
}

// ---------------- minimal JSON DOM ----------------
enum class JKind { Null, Bool, Num, Str, Arr, Obj };

struct JVal;
using JPair = std::pair<std::string, JVal>;

struct JVal {
  JKind kind = JKind::Null;
  bool b = false;
  std::string s;               // Str: decoded utf-8; Num: raw token
  std::vector<JVal> arr;
  std::vector<JPair> obj;
};

struct JParser {
  const char* p = nullptr;
  const char* end = nullptr;

  void SkipWs() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

  bool ParseString(std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    out.clear();
    while (p < end) {
      unsigned char c = (unsigned char)*p;
      if (c == '"') { ++p; break; }
      if (c == '\\') {
        ++p;
        if (p >= end) return false;
        char e = *p++;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            if (p + 4 > end) return false;
            unsigned int cp = 0;
            for (int i = 0; i < 4; ++i) {
              char h = *p++;
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
              else return false;
            }
            unsigned int v = cp;
            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
              const char* save = p;
              p += 2;
              unsigned int lo = 0;
              bool ok = true;
              for (int i = 0; i < 4; ++i) {
                char h = *p++;
                lo <<= 4;
                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                else { ok = false; break; }
              }
              if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                v = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                p = save;
              }
            }
            if (v < 0x80) {
              out += (char)v;
            } else if (v < 0x800) {
              out += (char)(0xC0 | (v >> 6));
              out += (char)(0x80 | (v & 0x3F));
            } else if (v < 0x10000) {
              out += (char)(0xE0 | (v >> 12));
              out += (char)(0x80 | ((v >> 6) & 0x3F));
              out += (char)(0x80 | (v & 0x3F));
            } else {
              out += (char)(0xF0 | (v >> 18));
              out += (char)(0x80 | ((v >> 12) & 0x3F));
              out += (char)(0x80 | ((v >> 6) & 0x3F));
              out += (char)(0x80 | (v & 0x3F));
            }
            break;
          }
          default: return false;
        }
      } else {
        out += (char)c;
        ++p;
      }
    }
    return true;
  }

  bool ParseValue(JVal& v) {
    SkipWs();
    if (p >= end) return false;
    if (*p == '{') {
      ++p;
      v.kind = JKind::Obj;
      SkipWs();
      if (p < end && *p == '}') { ++p; return true; }
      while (true) {
        SkipWs();
        std::string key;
        if (!ParseString(key)) return false;
        SkipWs();
        if (p >= end || *p != ':') return false;
        ++p;
        JVal val;
        if (!ParseValue(val)) return false;
        v.obj.emplace_back(std::move(key), std::move(val));
        SkipWs();
        if (p >= end) return false;
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; return true; }
        return false;
      }
    }
    if (*p == '[') {
      ++p;
      v.kind = JKind::Arr;
      SkipWs();
      if (p < end && *p == ']') { ++p; return true; }
      while (true) {
        JVal val;
        if (!ParseValue(val)) return false;
        v.arr.push_back(std::move(val));
        SkipWs();
        if (p >= end) return false;
        if (*p == ',') { ++p; continue; }
        if (*p == ']') { ++p; return true; }
        return false;
      }
    }
    if (*p == '"') {
      v.kind = JKind::Str;
      return ParseString(v.s);
    }
    if (p + 4 <= end && strncmp(p, "true", 4) == 0) { v.kind = JKind::Bool; v.b = true; p += 4; return true; }
    if (p + 5 <= end && strncmp(p, "false", 5) == 0) { v.kind = JKind::Bool; v.b = false; p += 5; return true; }
    if (p + 4 <= end && strncmp(p, "null", 4) == 0) { v.kind = JKind::Null; p += 4; return true; }
    const char* start = p;
    while (p < end && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) ++p;
    if (p > start) { v.kind = JKind::Num; v.s.assign(start, p - start); return true; }
    return false;
  }
};

static void EscapeJson(const std::string& in, std::string& out) {
  out += '"';
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char b[8];
          sprintf_s(b, "\\u%04x", c);
          out += b;
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}

static void Serialize(const JVal& v, std::string& out) {
  switch (v.kind) {
    case JKind::Null: out += "null"; break;
    case JKind::Bool: out += v.b ? "true" : "false"; break;
    case JKind::Num: out += v.s; break;
    case JKind::Str: EscapeJson(v.s, out); break;
    case JKind::Arr: {
      out += '[';
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i) out += ',';
        Serialize(v.arr[i], out);
      }
      out += ']';
      break;
    }
    case JKind::Obj: {
      out += '{';
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i) out += ',';
        EscapeJson(v.obj[i].first, out);
        out += ':';
        Serialize(v.obj[i].second, out);
      }
      out += '}';
      break;
    }
  }
}

// ---------------- rewrite logic ----------------
static JVal* FindKey(JVal& obj, const char* key) {
  for (auto& kv : obj.obj) if (kv.first == key) return &kv.second;
  return nullptr;
}

static const JVal* FindKey(const JVal& obj, const char* key) {
  for (const auto& kv : obj.obj) if (kv.first == key) return &kv.second;
  return nullptr;
}

static std::string GetStr(const JVal* v) { return (v && v->kind == JKind::Str) ? v->s : ""; }

static void Rewrite(JVal& v, bool& changed);

static void RewriteAgentMessage(JVal& obj, bool& changed) {
  JVal* contentPtr = FindKey(obj, "content");
  std::vector<JVal> newContent;
  std::string envelope;
  std::string taskText;
  if (contentPtr && contentPtr->kind == JKind::Arr) {
    for (auto& item : contentPtr->arr) {
      if (item.kind == JKind::Obj) {
        std::string t = GetStr(FindKey(item, "type"));
        if (t == "input_text") {
          std::string txt = GetStr(FindKey(item, "text"));
          if (txt.compare(0, 13, "Message Type:") == 0) envelope = txt;
          else newContent.push_back(item);
        } else if (t == "encrypted_content") {
          taskText = GetStr(FindKey(item, "encrypted_content"));
        } else {
          newContent.push_back(item);
        }
      } else {
        newContent.push_back(item);
      }
    }
  }
  std::string merged = envelope;
  if (!taskText.empty()) {
    if (!merged.empty()) merged += "\n";
    merged += taskText;
  }
  LogTask("task to " + GetStr(FindKey(obj, "recipient")) + ":\r\n" + merged + "\r\n---");
  JVal textItem;
  textItem.kind = JKind::Obj;
  textItem.obj.emplace_back("type", JVal{});
  textItem.obj.back().second.kind = JKind::Str;
  textItem.obj.back().second.s = "input_text";
  textItem.obj.emplace_back("text", JVal{});
  textItem.obj.back().second.kind = JKind::Str;
  textItem.obj.back().second.s = merged.empty() ? envelope : merged;
  newContent.insert(newContent.begin(), std::move(textItem));

  JVal msg;
  msg.kind = JKind::Obj;
  msg.obj.emplace_back("type", JVal{});
  msg.obj.back().second.kind = JKind::Str;
  msg.obj.back().second.s = "message";
  msg.obj.emplace_back("role", JVal{});
  msg.obj.back().second.kind = JKind::Str;
  msg.obj.back().second.s = "user";
  if (const JVal* id = FindKey(obj, "id")) msg.obj.emplace_back("id", *id);
  msg.obj.emplace_back("content", JVal{});
  msg.obj.back().second.kind = JKind::Arr;
  msg.obj.back().second.arr = std::move(newContent);

  obj = std::move(msg);
  changed = true;
}

static void Rewrite(JVal& v, bool& changed) {
  if (v.kind == JKind::Arr) {
    for (auto& item : v.arr) Rewrite(item, changed);
    return;
  }
  if (v.kind != JKind::Obj) return;

  const JVal* type = FindKey(v, "type");
  if (type && type->kind == JKind::Str && type->s == "agent_message") {
    RewriteAgentMessage(v, changed);
    return;
  }
  if (type && type->kind == JKind::Str && type->s == "encrypted_content") {
    const JVal* ec = FindKey(v, "encrypted_content");
    if (ec && ec->kind == JKind::Str) {
      JVal out;
      out.kind = JKind::Obj;
      out.obj.emplace_back("type", JVal{});
      out.obj.back().second.kind = JKind::Str;
      out.obj.back().second.s = "input_text";
      out.obj.emplace_back("text", *ec);
      v = std::move(out);
      changed = true;
    }
    return;
  }
  for (auto& kv : v.obj) Rewrite(kv.second, changed);
}

static bool RewriteBody(const std::string& in, std::string& out, bool& changed) {
  JParser parser;
  parser.p = in.data();
  parser.end = in.data() + in.size();
  JVal root;
  if (!parser.ParseValue(root)) return false;
  changed = false;
  Rewrite(root, changed);
  out.clear();
  Serialize(root, out);
  return true;
}

// ---------------- winsock helpers ----------------
static bool SendAll(SOCKET s, const char* data, int len) {
  int sent = 0;
  while (sent < len) {
    int n = send(s, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += n;
  }
  return true;
}

static void SendSimple(SOCKET s, int code, const char* contentType, const char* body) {
  char head[256];
  int hl = sprintf_s(head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                     code, code == 200 ? "OK" : "Bad Gateway", contentType, (int)strlen(body));
  SendAll(s, head, hl);
  SendAll(s, body, (int)strlen(body));
}

static void HandleConnection(SOCKET client) {
  std::string req;
  char buf[16384];
  size_t headerEnd = std::string::npos;
  while (true) {
    int n = recv(client, buf, sizeof(buf), 0);
    if (n <= 0) break;
    req.append(buf, n);
    headerEnd = req.find("\r\n\r\n");
    if (headerEnd != std::string::npos) break;
    if (req.size() > 1024 * 1024) break;
  }
  if (headerEnd == std::string::npos) {
    closesocket(client);
    return;
  }

  std::string head = req.substr(0, headerEnd);
  size_t sp1 = head.find(' ');
  size_t sp2 = sp1 == std::string::npos ? std::string::npos : head.find(' ', sp1 + 1);
  std::string method = sp1 == std::string::npos ? "" : head.substr(0, sp1);
  std::string path = (sp1 == std::string::npos || sp2 == std::string::npos) ? "/" : head.substr(sp1 + 1, sp2 - sp1 - 1);

  std::string auth, contentType, accept = "text/event-stream";
  long long contentLength = -1;
  std::string rest = head;
  while (true) {
    size_t pos = rest.find("\r\n");
    std::string line = (pos == std::string::npos) ? rest : rest.substr(0, pos);
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
      for (auto& c : name) if (c >= 'a' && c <= 'z') c -= 32;
      if (name == "CONTENT-LENGTH") contentLength = _strtoi64(value.c_str(), nullptr, 10);
      else if (name == "AUTHORIZATION") auth = value;
      else if (name == "CONTENT-TYPE") contentType = value;
      else if (name == "ACCEPT") accept = value;
    }
    if (pos == std::string::npos) break;
    rest.erase(0, pos + 2);
  }

  if (method == "GET" && (path == "/__ping" || path == "/__health")) {
    if (path == "/__health") {
      std::string hb = std::string("{\"status\":\"ok\",\"version\":\"") + kVersion +
                       "\",\"port\":" + std::to_string(g_port) +
                       ",\"upstream\":\"https://api.deepseek.com\"}";
      SendSimple(client, 200, "application/json", hb.c_str());
    } else {
      SendSimple(client, 200, "text/plain", "ok");
    }
    closesocket(client);
    return;
  }

  std::string body;
  if (contentLength > 0) {
    size_t have = req.size() - (headerEnd + 4);
    body = req.substr(headerEnd + 4);
    while ((long long)body.size() < contentLength) {
      int n = recv(client, buf, sizeof(buf), 0);
      if (n <= 0) break;
      body.append(buf, n);
    }
    body.resize((size_t)contentLength);
  }

  std::string outBody = body;
  bool changed = false;
  if (method == "POST" && !body.empty()) {
    std::string rewritten;
    if (RewriteBody(body, rewritten, changed)) outBody = rewritten;
  }
  if (changed) Log(("rewrote agent message: " + method + " " + path).c_str());

  HINTERNET hSession = WinHttpOpen(L"ds-adapter/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    SendSimple(client, 502, "text/plain", "adapter: WinHttpOpen failed");
    closesocket(client);
    return;
  }
  HINTERNET hConnect = WinHttpConnect(hSession, kUpstreamHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    SendSimple(client, 502, "text/plain", "adapter: WinHttpConnect failed");
    WinHttpCloseHandle(hSession);
    closesocket(client);
    return;
  }
  std::wstring wpath(path.begin(), path.end());
  HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!hReq) {
    SendSimple(client, 502, "text/plain", "adapter: WinHttpOpenRequest failed");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    closesocket(client);
    return;
  }

  std::wstring wAuth(auth.begin(), auth.end());
  if (!wAuth.empty()) {
    std::wstring hdr = L"Authorization: " + wAuth;
    WinHttpAddRequestHeaders(hReq, hdr.c_str(), (DWORD)hdr.size(), WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
  }
  if (!contentType.empty()) {
    std::wstring hdr(contentType.begin(), contentType.end());
    hdr = L"Content-Type: " + hdr;
    WinHttpAddRequestHeaders(hReq, hdr.c_str(), (DWORD)hdr.size(), WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
  }
  if (!accept.empty()) {
    std::wstring hdr(accept.begin(), accept.end());
    hdr = L"Accept: " + hdr;
    WinHttpAddRequestHeaders(hReq, hdr.c_str(), (DWORD)hdr.size(), WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
  }

  BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               (void*)outBody.data(), (DWORD)outBody.size(), (DWORD)outBody.size(), 0);
  if (!ok) {
    Log(("upstream send failed, winhttp error=" + std::to_string(GetLastError())).c_str());
    SendSimple(client, 502, "text/plain", "adapter: upstream request failed");
  } else {
    ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) Log(("upstream receive failed, winhttp error=" + std::to_string(GetLastError())).c_str());
  }
  if (ok) {
    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
    std::string statusText = status == 200 ? "OK" : "Error";
    char reason[32];
    DWORD reasonLen = sizeof(reason);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX,
                            reason, &reasonLen, WINHTTP_NO_HEADER_INDEX)) {
      std::string tmp(reason, reasonLen);
      size_t z = tmp.find('\0');
      statusText = (z == std::string::npos) ? tmp : tmp.substr(0, z);
    }
    std::string upstreamCtype = "text/event-stream; charset=utf-8";
    char ctype[256];
    DWORD ctypeLen = sizeof(ctype);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            ctype, &ctypeLen, WINHTTP_NO_HEADER_INDEX)) {
      std::string tmp(ctype, ctypeLen);
      size_t z = tmp.find('\0');
      upstreamCtype = (z == std::string::npos) ? tmp : tmp.substr(0, z);
    }
    Log(("req " + method + " " + path + " -> " + std::to_string(status)).c_str());
    char head[256];
    int hl = sprintf_s(head, "HTTP/1.1 %lu %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                       status, statusText.c_str(), upstreamCtype.c_str());
    SendAll(client, head, hl);
    char rbuf[32768];
    while (true) {
      DWORD read = 0;
      if (!WinHttpReadData(hReq, rbuf, sizeof(rbuf), &read) || read == 0) break;
      if (!SendAll(client, rbuf, (int)read)) break;
    }
  }

  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  closesocket(client);
}

static bool PortOpen(const char* host, int port, int timeoutMs) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  inet_pton(AF_INET, host, &addr.sin_addr);
  bool open = false;
  if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
    open = true;
  } else {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(0, nullptr, &wfds, nullptr, &tv) > 0) {
      int err = 0;
      int errLen = sizeof(err);
      getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &errLen);
      open = (err == 0);
    }
  }
  closesocket(s);
  return open;
}

static void RunWatchdog() {
  if (PortOpen("127.0.0.1", g_port, 1000)) return;
  std::wstring cmdline = L"\"" + g_exePath + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(g_exePath.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

static bool UpstreamReachable() {
  HINTERNET hSession = WinHttpOpen(L"ds-adapter/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return false;
  HINTERNET hConnect = WinHttpConnect(hSession, kUpstreamHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    return false;
  }
  HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  bool reachable = false;
  if (hReq) {
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
      if (WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD status = 0;
        DWORD statusLen = sizeof(status);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                                WINHTTP_NO_HEADER_INDEX)) {
          reachable = true;  // any HTTP status proves TLS + HTTP reachability
        }
      }
    }
    WinHttpCloseHandle(hReq);
  }
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return reachable;
}

static int RunSelfTest() {
  bool local = PortOpen("127.0.0.1", g_port, 1500);
  bool upstream = UpstreamReachable();
  std::string result;
  result += "local_127.0.0.1:" + std::to_string(g_port) + "=" +
            (local ? "listening" : "not_listening") + "\r\n";
  result += "upstream=https://api.deepseek.com reachable=" +
            std::string(upstream ? "yes" : "no") + "\r\n";
  result += "selftest=" + std::string(local && upstream ? "OK" : "FAILED") + "\r\n";
  std::wstring outPath = g_exeDir + L"selftest.log";
  FILE* f = nullptr;
  if (_wfopen_s(&f, outPath.c_str(), L"wb") == 0 && f) {
    fwrite(result.data(), 1, result.size(), f);
    fclose(f);
  }
  return (local && upstream) ? 0 : 1;
}

static void RunServer() {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Log("adapter: WSAStartup failed");
    return;
  }
  SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenSock == INVALID_SOCKET) {
    Log("adapter: socket failed");
    WSACleanup();
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)g_port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
    Log("adapter: bind failed (port in use?)");
    closesocket(listenSock);
    WSACleanup();
    return;
  }
  if (listen(listenSock, 16) != 0) {
    Log("adapter: listen failed");
    closesocket(listenSock);
    WSACleanup();
    return;
  }
  Log(("listening on http://127.0.0.1:" + std::to_string(g_port) + " -> https://api.deepseek.com").c_str());
  while (true) {
    SOCKET client = accept(listenSock, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    std::thread([client]() { HandleConnection(client); }).detach();
  }
}

int main(int argc, char** argv) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g_exePath = exe;
  size_t slash = g_exePath.find_last_of(L"\\/");
  g_exeDir = (slash == std::wstring::npos) ? L"." : g_exePath.substr(0, slash + 1);
  g_logPath = g_exeDir + L"adapter.log";
  g_taskLogPath = g_exeDir + L"tasks.log";

  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);

  int argi = 1;
  if (argc > argi && argv[argi][0] >= '0' && argv[argi][0] <= '9') {
    int p = atoi(argv[argi]);
    if (p >= 1 && p <= 65535) g_port = p;
    ++argi;
  }

  if (argc > argi && strcmp(argv[argi], "watchdog") == 0) {
    RunWatchdog();
    WSACleanup();
    return 0;
  }
  if (argc > argi && strcmp(argv[argi], "selftest") == 0) {
    int rc = RunSelfTest();
    WSACleanup();
    return rc;
  }
  if (argc > argi + 2 && strcmp(argv[argi], "test") == 0) {
    FILE* f = nullptr;
    if (fopen_s(&f, argv[argi + 1], "rb") == 0 && f) {
      std::string in;
      char b[65536];
      size_t n;
      while ((n = fread(b, 1, sizeof(b), f)) > 0) in.append(b, n);
      fclose(f);
      std::string out;
      bool changed = false;
      if (RewriteBody(in, out, changed)) {
        FILE* fo = nullptr;
        if (fopen_s(&fo, argv[argi + 2], "wb") == 0 && fo) {
          fwrite(out.data(), 1, out.size(), fo);
          fclose(fo);
          return changed ? 0 : 2;
        }
      }
    }
    return 1;
  }

  RunServer();
  WSACleanup();
  return 0;
}
