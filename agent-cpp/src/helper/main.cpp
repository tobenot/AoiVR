// aoi-sandbox-helper: the ONLY place the agent executes model-controlled
// writes. Runs as the dedicated sandbox user; every model-controlled COMMAND
// (op=bash) is spawned through a WRITE_RESTRICTED restricted token derived
// from the helper's own token, so the OS enforces that writes can only happen
// inside the sandbox workspace (capability-SID-gated), everything else is
// denied by the token's restricting SIDs (Codex elevated-backend semantics:
// the helper plays the role of codex-command-runner.exe).
//
// Protocol: reads ONE JSON document from stdin (to EOF):
//   {"op":"bash","command":"..."}
//   {"op":"read","path":"..."}
//   {"op":"write","path":"...","content":"..."}
//   {"op":"edit","path":"...","old_string":"...","new_string":"..."}
//   {"op":"sql_query","sql":"...","db_path":"..."}
//   {"op":"convert","fn":"base64_decode|base64_encode|json_query",
//    "value":"...","path":"..."}
// Replies on stdout with one JSON document:
//   {"ok":true,"output":"..."} | {"ok":false,"error":"..."}
// argv: helper.exe <workspace> [<capabilitySid>]  (capsid enables the
// restricted spawn; without it bash falls back to a plain spawn).
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "base64.h"
#include "nlohmann/json.hpp"
#include "agent_utils.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string readAllStdin() {
  std::ostringstream ss;
  char buf[4096];
  size_t total = 0;
  constexpr size_t kMaxStdin = 16 * 1024 * 1024;  // bound the op JSON
  size_t n = 0;
  do {
    std::cin.read(buf, sizeof(buf));
    n = static_cast<size_t>(std::cin.gcount());
    if (n > 0) {
      const size_t room = kMaxStdin - (std::min)(kMaxStdin, total);
      if (room == 0) break;
      const size_t take = (std::min)(room, n);
      ss.write(buf, static_cast<std::streamsize>(take));
      total += take;
    }
  } while (n > 0);
  return ss.str();
}

// Reject paths that could escape the intended root: absolute paths, drive
// letters, UNC roots and any ".." component. write/edit rely on this; read
// keeps absolute paths (sandbox user's own permissions apply) but its exeDir
// fallback must also pass this check.
bool isSafeRelativePath(const std::string& path) {
  if (path.empty()) return false;
  if (path.size() >= 2 && path[1] == ':') return false;      // drive letter
  if (path[0] == '\\' || path[0] == '/') return false;       // absolute/UNC
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    const char ch = i < path.size() ? path[i] : '\0';
    if (ch == '\\' || ch == '/' || ch == '\0') {
      if (cur == "..") return false;
      cur.clear();
    } else {
      cur += ch;
    }
  }
  return true;
}

std::string readTextFile(const std::string& path, size_t offset, size_t maxBytes) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return "(file not found: " + path + ")";
  const size_t total = static_cast<size_t>(f.tellg());
  if (offset >= total && total > 0)
    return "(read: offset " + std::to_string(offset) +
           " beyond end of file; total " + std::to_string(total) + " bytes)";
  f.seekg(static_cast<std::streamoff>(offset));
  // Bounded read: never pull a huge file fully into memory (a multi-GB file
  // would OOM the helper before the truncation below could apply).
  std::string out;
  out.reserve((std::min)(maxBytes, static_cast<size_t>(4096)));
  char buf[8192];
  size_t readBytes = 0;
  while (f && readBytes < maxBytes) {
    const size_t want = (std::min)(sizeof(buf), maxBytes - readBytes);
    f.read(buf, static_cast<std::streamsize>(want));
    const std::streamsize n = f.gcount();
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
    readBytes += static_cast<size_t>(n);
  }
  // Nothing is discarded: if more remains, say exactly where to continue.
  if (offset + readBytes < total) {
    const size_t complete = aoi::utf8CompleteLength(out);
    out.resize(complete);
    out += "\n...(total " + std::to_string(total) + " bytes; continue with offset=" +
           std::to_string(offset + complete) + ")";
  }
  return out;
}

// Save the FULL tool output into the workspace out\ dir so nothing is lost
// when only a size-capped head is returned to the model. Returns the
// workspace-relative path, or "" on failure.
std::string saveFullOutput(const std::string& op, const std::string& content) {
  std::error_code ec;
  fs::create_directories("out", ec);
  const std::string name =
      "out\\" + op + "_" + std::to_string(GetTickCount64()) + ".txt";
  std::ofstream f(name, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return "";
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  f.close();
  return name;
}

// ---- bash ----
// Agent exe dir (argv[3]): used to resolve read paths relative to shipped
// files (e.g. docs/...) when they don't exist inside the sandbox workspace.
std::string g_exeDir;
// The command runs through a WRITE_RESTRICTED restricted token (built once
// from the helper's own token): restricting SIDs = [capability SID, sandbox
// user SID, logon SID, Everyone]. Writes are OS-enforced to paths whose DACL
// grants the capability SID - i.e. the sandbox workspace - regardless of how
// permissive other directories are. Mirrors Codex token.rs.
//
// KNOWN LIMITATION: schannel/TLS (curl https, PowerShell Invoke-WebRequest)
// fails inside WRITE_RESTRICTED children with SEC_E_NO_CREDENTIALS
// (0x8009030e) - a Windows behavior that also affects Codex's sandbox (open
// issue openai/codex#17459). The fetch tool provides network access instead
// (it runs in the agent process where schannel works).
HANDLE g_restrictedToken = nullptr;

// Enable a privilege on a token if the token holds it (disabled -> enabled).
void enablePrivilege(HANDLE tok, const char* name) {
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueA(nullptr, name, &tp.Privileges[0].Luid)) return;
  AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
}

bool initRestrictedToken(const char* capSidStr) {
  HANDLE base = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES |
                            TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT |
                            TOKEN_ADJUST_SESSIONID,
                        &base))
    return false;
  // The sandbox user account is granted SeAssignPrimaryToken/SeIncreaseQuota
  // by the elevated setup; make sure they are enabled on this process token
  // (CreateProcessAsUserW checks the CALLING process's token).
  enablePrivilege(base, "SeAssignPrimaryTokenPrivilege");
  enablePrivilege(base, "SeIncreaseQuotaPrivilege");

  PSID capSid = nullptr;
  if (!ConvertStringSidToSidA(capSidStr, &capSid)) {
    CloseHandle(base);
    return false;
  }

  // Restricting SIDs: capability SID, sandbox user SID, logon SID, Everyone.
  BYTE worldBuf[SECURITY_MAX_SID_SIZE]{};
  DWORD worldLen = sizeof(worldBuf);
  CreateWellKnownSid(WinWorldSid, nullptr, worldBuf, &worldLen);
  std::vector<BYTE> userBuf(SECURITY_MAX_SID_SIZE);
  std::vector<BYTE> logonBuf(SECURITY_MAX_SID_SIZE);
  {
    // TokenUser
    DWORD len = 0;
    GetTokenInformation(base, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> tu(len);
    if (GetTokenInformation(base, TokenUser, tu.data(), len, &len)) {
      PSID sid = reinterpret_cast<TOKEN_USER*>(tu.data())->User.Sid;
      userBuf.assign(static_cast<size_t>(GetLengthSid(sid)), 0);
      CopySid(static_cast<DWORD>(userBuf.size()), userBuf.data(), sid);
    }
    // TokenGroups: find the logon SID
    len = 0;
    GetTokenInformation(base, TokenGroups, nullptr, 0, &len);
    std::vector<BYTE> tg(len);
    if (GetTokenInformation(base, TokenGroups, tg.data(), len, &len)) {
      const DWORD count = reinterpret_cast<TOKEN_GROUPS*>(tg.data())->GroupCount;
      for (DWORD i = 0; i < count; ++i) {
        const SID_AND_ATTRIBUTES& g =
            reinterpret_cast<TOKEN_GROUPS*>(tg.data())->Groups[i];
        if (g.Attributes & SE_GROUP_LOGON_ID) {
          logonBuf.assign(static_cast<size_t>(GetLengthSid(g.Sid)), 0);
          CopySid(static_cast<DWORD>(logonBuf.size()), logonBuf.data(), g.Sid);
          break;
        }
      }
    }
  }

  SID_AND_ATTRIBUTES restricting[4]{};
  int n = 0;
  restricting[n].Sid = capSid;
  ++n;
  if (!userBuf.empty()) { restricting[n].Sid = userBuf.data(); ++n; }
  if (!logonBuf.empty()) { restricting[n].Sid = logonBuf.data(); ++n; }
  restricting[n].Sid = reinterpret_cast<PSID>(worldBuf);
  ++n;

  HANDLE restricted = nullptr;
  const BOOL ok = CreateRestrictedToken(
      base, DISABLE_MAX_PRIVILEGE | LUA_TOKEN | WRITE_RESTRICTED, 0, nullptr, 0,
      nullptr, static_cast<DWORD>(n), restricting, &restricted);
  CloseHandle(base);
  if (!ok || !restricted) {
    LocalFree(capSid);
    return false;
  }

  // Default DACL: grant logon SID + Everyone + capability SID so sandboxed
  // processes can create pipes / IPC objects (Codex token.rs).
  {
    EXPLICIT_ACCESSA eas[3]{};
    int ea = 0;
    auto addEa = [&](PSID sid) {
      if (!sid) return;
      eas[ea].grfAccessMode = GRANT_ACCESS;
      eas[ea].grfAccessPermissions = GENERIC_ALL;
      eas[ea].grfInheritance = NO_INHERITANCE;
      eas[ea].Trustee.TrusteeForm = TRUSTEE_IS_SID;
      eas[ea].Trustee.ptstrName = reinterpret_cast<LPSTR>(sid);
      ++ea;
    };
    addEa(logonBuf.empty() ? nullptr : reinterpret_cast<PSID>(logonBuf.data()));
    addEa(reinterpret_cast<PSID>(worldBuf));
    addEa(capSid);
    PACL dacl = nullptr;
    if (SetEntriesInAclA(ea, eas, nullptr, &dacl) == ERROR_SUCCESS && dacl) {
      SetTokenInformation(restricted, TokenDefaultDacl, &dacl, sizeof(PACL));
      LocalFree(dacl);
    }
  }
  // capSid is no longer referenced after the default-DACL block above.
  LocalFree(capSid);

  // Re-enable directory traversal (disabled by DISABLE_MAX_PRIVILEGE).
  enablePrivilege(restricted, "SeChangeNotifyPrivilege");

  g_restrictedToken = restricted;
  return true;
}

// UTF-8 -> UTF-16 (the op JSON from the agent is UTF-8; a byte-wise widen
// would corrupt CJK command lines).
std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.pop_back();
  return w;
}

// Run a command with the restricted token when available; falls back to a
// plain spawn (restricted token unavailable, e.g. no capsid passed).
std::string runBash(const std::string& cmd) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE hRead = nullptr, hWrite = nullptr;
  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "(CreatePipe failed)";
  SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;
  si.dwFlags = STARTF_USESTDHANDLES;
  PROCESS_INFORMATION pi{};
  const std::string fullCmd = "cmd.exe /c " + cmd;
  bool spawned = false;
  if (g_restrictedToken) {
    // Wide-char spawn through the restricted token (Codex process.rs).
    const std::wstring cmdW = utf8ToWide(fullCmd);
    std::vector<wchar_t> cmdBuf(cmdW.begin(), cmdW.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW siW{};
    siW.cb = sizeof(siW);
    siW.hStdOutput = hWrite;
    siW.hStdError = hWrite;
    siW.dwFlags = STARTF_USESTDHANDLES;
    if (CreateProcessAsUserW(g_restrictedToken, nullptr, cmdBuf.data(), nullptr,
                             nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &siW, &pi)) {
      spawned = true;
    } else {
      const DWORD err = GetLastError();
      if (err == ERROR_PRIVILEGE_NOT_HELD || err == ERROR_ACCESS_DENIED) {
        std::fprintf(stderr,
                     "(sandbox helper: restricted spawn failed (%lu); "
                     "falling back to plain spawn - write isolation degraded)\n",
                     err);
      }
    }
  }
  if (!spawned) {
    std::vector<char> cmdBuf(fullCmd.begin(), fullCmd.end());
    cmdBuf.push_back('\0');
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      CloseHandle(hRead);
      CloseHandle(hWrite);
      return "(CreateProcess failed)";
    }
  }
  CloseHandle(hWrite);

  // Stream the child's output: the first 30KB stay in memory as the reply
  // head; anything beyond that is streamed to out\bash_<ts>.txt (head first,
  // then the tail) so NOTHING is lost and the process is never killed for
  // producing a lot of output.
  constexpr size_t kHead = 30000;
  std::string out;
  std::string spillPath;
  std::ofstream spill;
  size_t totalBytes = 0;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
    size_t off = 0;
    if (out.size() < kHead) {
      const size_t take = (std::min)(kHead - out.size(), static_cast<size_t>(n));
      out.append(buf, take);
      off = take;
    }
    if (off < static_cast<size_t>(n)) {
      if (!spill.is_open()) {
        std::error_code ec;
        fs::create_directories("out", ec);
        spillPath = "out\\bash_" + std::to_string(GetTickCount64()) + ".txt";
        spill.open(spillPath, std::ios::binary | std::ios::trunc);
        if (spill.is_open())
          spill.write(out.data(), static_cast<std::streamsize>(out.size()));
      }
      if (spill.is_open())
        spill.write(buf + off, static_cast<std::streamsize>(n - off));
    }
    totalBytes += n;
  }
  if (spill.is_open()) spill.close();
  CloseHandle(hRead);
  WaitForSingleObject(pi.hProcess, 5000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (out.empty()) {
    // PowerShell/pwsh under the sandbox user fail to start (USER32-dependent:
    // the restricted desktop can't host them) and produce NO output. Surface
    // that immediately instead of letting the model retry it four times
    // (observed in regression: 4 wasted turns before switching tools).
    if (cmd.find("powershell") != std::string::npos ||
        cmd.find("pwsh") != std::string::npos) {
      out = "(no output: PowerShell cannot run under the sandbox user - use "
            "convert json_query_file for JSON stats instead)";
    } else {
      out = "(no output)";
    }
  }
  if (!spillPath.empty() && totalBytes > kHead) {
    aoi::utf8SafeTruncate(out, kHead);
    out += "\n...(truncated: full output " + std::to_string(totalBytes) +
           " bytes saved to " + spillPath + "; use read with offset to page through)";
  }
  return out;
}

// ---- sql_query (read-only, path-whitelisted) ----
std::string trimLeft(const std::string& s) {
  const size_t pos = s.find_first_not_of(" \t\r\n");
  return pos == std::string::npos ? std::string() : s.substr(pos);
}

// Normalize a path for comparison: lowercase, forward slashes, strip trailing
// separators/dots - so "C:\...\VRCX.sqlite3" matches "c:/.../vr cx..." forms.
std::string normPathForCompare(const std::string& p) {
  std::string n = p;
  for (auto& c : n) {
    if (c == '\\') c = '/';
    else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  while (n.size() > 1 && (n.back() == '/' || n.back() == '.')) n.pop_back();
  return n;
}

std::string runSqlQuery(const std::string& path, const std::string& sql,
                        const std::string& allowedDb) {
  const std::string sqlT = trimLeft(sql);
  auto kwEndOk = [&sqlT](const char* kw, size_t len) {
    if (sqlT.rfind(kw, 0) != 0) return false;
    if (sqlT.size() == len) return true;
    const unsigned char c = static_cast<unsigned char>(sqlT[len]);
    return isspace(c) != 0 || c == '"' || c == '\'' || c == '(';
  };
  const bool select = kwEndOk("SELECT", 6) || kwEndOk("select", 6) ||
                      kwEndOk("WITH", 4) || kwEndOk("with", 4);
  const bool pragma = kwEndOk("PRAGMA", 6) || kwEndOk("pragma", 6);
  if (!select && !pragma)
    return "(sql_query rejected: only SELECT/WITH/PRAGMA statements are allowed)";
  // ATTACH could open another database even under SQLITE_OPEN_READONLY.
  if (sqlT.find("ATTACH") != std::string::npos ||
      sqlT.find("attach") != std::string::npos)
    return "(sql_query rejected: ATTACH is not allowed)";

  // Defense in depth: only the whitelisted database path may be queried (the
  // sandbox user is world-readable, so an unrestricted path would expose any
  // .sqlite on disk - e.g. other apps' credentials).
  if (!allowedDb.empty()) {
    if (normPathForCompare(path) != normPathForCompare(allowedDb))
      return "(sql_query rejected: db_path is not the allowed VRCX database)";
  }

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    std::string err = db ? sqlite3_errmsg(db) : "cannot open database";
    if (db) sqlite3_close(db);
    return "(sql_query failed: " + err + ")";
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    std::string err = sqlite3_errmsg(db);
    sqlite3_close(db);
    return "(sql_query failed: " + err + ")";
  }
  const int nCols = sqlite3_column_count(stmt);
  std::vector<std::string> colNames;
  for (int i = 0; i < nCols; ++i) {
    const char* name = sqlite3_column_name(stmt, i);
    colNames.emplace_back(name ? name : "");
  }
  json rows = json::array();
  int rc = SQLITE_OK;
  // Bound the result set: a million-row query must not be materialized fully
  // in memory before the byte cap below applies.
  constexpr size_t kMaxRows = 1000;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (rows.size() >= kMaxRows) {
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      json out = {{"db", path},
                  {"count", ">" + std::to_string(kMaxRows)},
                  {"rows", rows}};
      return out.dump();
    }
    json row = json::object();
    for (int i = 0; i < nCols; ++i) {
      const std::string& name = colNames[static_cast<size_t>(i)];
      switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER: row[name] = sqlite3_column_int64(stmt, i); break;
        case SQLITE_FLOAT: row[name] = sqlite3_column_double(stmt, i); break;
        case SQLITE_NULL: row[name] = nullptr; break;
        default: {
          const unsigned char* text = sqlite3_column_text(stmt, i);
          row[name] = text ? reinterpret_cast<const char*>(text) : "";
          break;
        }
      }
    }
    rows.push_back(std::move(row));
  }
  std::string err;
  if (rc != SQLITE_DONE) err = sqlite3_errmsg(db);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (!err.empty()) return "(sql_query failed: " + err + ")";

  json out = {{"db", path}, {"count", rows.size()}, {"rows", rows}};
  std::string text = out.dump();
  constexpr size_t kMax = 30000;
  if (text.size() > kMax) {
    const size_t fullSize = text.size();
    const std::string saved = saveFullOutput("sql", text);
    aoi::utf8SafeTruncate(text, kMax);
    if (!saved.empty())
      text += "\n...(truncated: full output " + std::to_string(fullSize) +
              " bytes saved to " + saved + "; use read with offset to page through)";
    else
      text += "\n...(truncated)";
  }
  return text;
}

// ---- convert (base64 / json_query) ----
json resolvePath(const json& root, const std::string& path) {
  json cur = root;
  std::string seg;
  for (size_t i = 0; i <= path.size(); ++i) {
    const char ch = i < path.size() ? path[i] : '\0';
    if (ch == '.' || ch == '\0') {
      if (!seg.empty()) {
        if (seg[0] == '[' && seg.back() == ']') {
          const int idx = std::atoi(seg.substr(1, seg.size() - 2).c_str());
          if (cur.is_array() && idx >= 0 && idx < static_cast<int>(cur.size()))
            cur = cur[static_cast<size_t>(idx)];
          else
            return json();
        } else if (cur.is_object() && cur.contains(seg)) {
          cur = cur[seg];
        } else {
          return json();
        }
        seg.clear();
      }
    } else {
      seg += ch;
    }
  }
  return cur;
}

std::string runConvert(const std::string& fn, const std::string& value,
                       const std::string& path, const std::string& filter,
                       bool count) {
  try {
    if (fn == "base64_decode") {
      std::string out;
      if (!Base64::Decode(value, &out)) return "(convert: invalid base64)";
      return out;
    }
    if (fn == "base64_encode") {
      std::string out;
      if (!Base64::Encode(value, &out)) return "(convert: encode failed)";
      return out;
    }
    if (fn == "json_query") {
      json root = json::parse(value, nullptr, false);
      if (root.is_discarded()) return "(convert: invalid JSON document)";
      json v = path.empty() ? root : resolvePath(root, path);
      if (v.is_string()) return v.get<std::string>();
      if (v.is_null()) return "null";
      return v.dump();
    }
    if (fn == "json_query_file") {
      // Read a JSON document from a workspace file and query/filter/count it.
      // This is the sandbox-safe way to process large API dumps saved by
      // fetch/bash/sql_query (PowerShell/python are NOT available under the
      // sandbox user - USER32-dependent processes fail to start).
      std::ifstream f(value, std::ios::binary);
      if (!f.is_open()) return "(convert: file not found: " + value + ")";
      std::string doc((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
      json root = json::parse(doc, nullptr, false);
      if (root.is_discarded())
        return "(convert: invalid JSON document: " + value + ")";
      json v = path.empty() ? root : resolvePath(root, path);
      // Optional filter on an array: "key==value" or "key!=value" (string
      // compare against each element's field; non-string values compare by
      // their JSON dump).
      if (!filter.empty() && v.is_array()) {
        const size_t eq = filter.find("==");
        const size_t ne = filter.find("!=");
        const bool isNe = ne != std::string::npos;
        const size_t sep = isNe ? ne : eq;
        if (sep != std::string::npos) {
          const std::string key = filter.substr(0, sep);
          const std::string want = filter.substr(sep + 2);
          json filtered = json::array();
          for (const auto& e : v) {
            if (!e.is_object() || !e.contains(key)) continue;
            const auto& ev = e[key];
            const std::string evs =
                ev.is_string() ? ev.get<std::string>() : ev.dump();
            const bool match = evs == want;
            if (isNe ? !match : match) filtered.push_back(e);
          }
          v = filtered;
        }
      }
      if (count) return std::to_string(v.is_array() ? v.size() : 1);
      if (v.is_string()) return v.get<std::string>();
      if (v.is_null()) return "null";
      return v.dump();
    }
    return "(convert: unknown fn '" + fn + "')";
  } catch (const std::exception& ex) {
    return std::string("(convert failed: ") + ex.what() + ")";
  }
}

} // namespace

int main(int argc, char** argv) {
  // Suppress GUI error dialogs (e.g. "whoami.exe - 应用程序无法正常启动
  // (0xc0000142)") for this process and everything it spawns (cmd, scripts).
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  // CreateProcessWithLogonW does not honor lpCurrentDirectory reliably, so
  // the agent passes the sandbox workspace as argv[1]; set it explicitly so
  // relative paths (bash, write, edit) stay inside the workspace.
  if (argc >= 2 && *argv[1]) {
    SetCurrentDirectoryA(argv[1]);
  }
  // argv[2] = capability SID (agent-provided): builds the WRITE_RESTRICTED
  // token used for op=bash children. Without it bash degrades to a plain
  // spawn (still the sandbox user, but without write gating).
  if (argc >= 3 && *argv[2]) {
    if (!initRestrictedToken(argv[2])) {
      std::fprintf(stderr,
                   "(sandbox helper: initRestrictedToken failed; bash runs "
                   "without write isolation)\n");
    }
  }
  // argv[3] = agent exe dir (for read fallback to shipped files).
  if (argc >= 4 && *argv[3]) {
    g_exeDir = argv[3];
  }
  const std::string input = readAllStdin();
  json req = json::parse(input, nullptr, false);
  if (req.is_discarded() || !req.is_object() || !req.contains("op") ||
      !req["op"].is_string()) {
    std::cout << R"json({"ok":false,"error":"(sandbox helper: bad request)"})json" << std::endl;
    return 1;
  }
  const std::string op = req["op"].get<std::string>();
  std::string output;
  std::string error;
  try {
    if (op == "bash") {
      output = runBash(req.value("command", ""));
    } else if (op == "read") {
      const std::string path = req.value("path", "");
      const size_t offset = static_cast<size_t>(req.value("offset", 0));
      const size_t maxB = static_cast<size_t>(req.value("max_bytes", 60000));
      output = readTextFile(path, offset, maxB);
      // Shipped knowledge files (e.g. docs/...) live next to the AGENT exe,
      // outside the sandbox workspace. When the relative path does not exist
      // in the workspace, resolve it against the exe dir (read-only fallback;
      // write/edit NEVER follow this path). The fallback only applies to safe
      // relative paths - a ".." path could escape the exe dir.
      if (output.rfind("(file not found", 0) == 0 && !g_exeDir.empty() &&
          isSafeRelativePath(path)) {
        const std::string alt = readTextFile(g_exeDir + "\\" + path, offset, maxB);
        if (alt.rfind("(file not found", 0) != 0) output = alt;
        else
          output = "(file not found: " + path + "; fallback " + g_exeDir + "\\" +
                   path + " also missing)";
      }
    } else if (op == "write") {
      const std::string path = req.value("path", "");
      const std::string content = req.value("content", "");
      // Bound the write size: the model could otherwise exhaust disk (and the
      // op JSON itself is unbounded in the agent->helper pipe).
      constexpr size_t kMaxWriteBytes = 8 * 1024 * 1024;
      if (!isSafeRelativePath(path)) {
        error = "(write rejected: path must be relative to the sandbox workspace)";
      } else if (content.size() > kMaxWriteBytes) {
        error = "(write rejected: content exceeds 8MB limit)";
      } else {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
          error = "(cannot write file: " + path + ")";
        } else {
          out.write(content.data(), static_cast<std::streamsize>(content.size()));
          out.close();
          output = "File written.";
        }
      }
    } else if (op == "edit") {
      const std::string path = req.value("path", "");
      const std::string oldS = req.value("old_string", "");
      const std::string newS = req.value("new_string", "");
      if (!isSafeRelativePath(path)) {
        error = "(edit rejected: path must be relative to the sandbox workspace)";
      } else {
      std::ifstream in(path, std::ios::binary);
      if (!in.is_open()) {
        error = "(file not found: " + path + ")";
      } else {
        std::ostringstream ss;
        ss << in.rdbuf();
        in.close();
        std::string text = ss.str();
        constexpr size_t kMaxEditBytes = 10 * 1024 * 1024;
        if (text.size() > kMaxEditBytes) {
          error = "(edit rejected: file exceeds 10MB limit)";
        } else {
          const size_t pos = text.find(oldS);
          if (pos == std::string::npos) {
            error = "(old_string not found in file)";
          } else {
            text.replace(pos, oldS.size(), newS);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
              error = "(cannot write file: " + path + ")";
            } else {
              out.write(text.data(), static_cast<std::streamsize>(text.size()));
              out.close();
              output = "Edit applied.";
            }
          }
        }
      }
      }
    } else if (op == "sql_query") {
      output = runSqlQuery(req.value("db_path", ""), req.value("sql", ""),
                           req.value("allowed_db", ""));
    } else if (op == "convert") {
      output = runConvert(req.value("fn", ""), req.value("value", ""),
                          req.value("path", ""), req.value("filter", ""),
                          req.value("count", false));
    } else {
      error = "(sandbox helper: unknown op '" + op + "')";
    }
  } catch (const std::exception& ex) {
    error = std::string("(sandbox helper: ") + ex.what() + ")";
  }

  json reply = json::object();
  if (error.empty()) {
    reply["ok"] = true;
    reply["output"] = output;
  } else {
    reply["ok"] = false;
    reply["error"] = error;
  }
  // Serialize defensively: a malformed payload (e.g. dangling UTF-8 that
  // slipped past sanitizers) must surface as an error reply, NOT escape the
  // try block above and abort the helper (0xC0000409 - "no output").
  try {
    std::cout << reply.dump() << std::endl;
  } catch (const std::exception& ex) {
    std::cout << R"json({"ok":false,"error":"(sandbox helper: reply serialization failed)"})json"
              << std::endl;
  }
  return 0;
}
