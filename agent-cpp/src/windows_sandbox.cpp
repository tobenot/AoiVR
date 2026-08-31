#include "windows_sandbox.hpp"

#include <windows.h>

#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "base64.h"

namespace aoi {

// Forward declaration in the DIRECT aoi scope (NOT inside the anonymous
// namespace below): the anonymous namespace's implicit using-directive would
// otherwise create a second same-named entity and make every call ambiguous.
// Defined below; used by runSandboxSetup for the wide-char setup path/args
// (the ANSI module path is GBK on CJK systems - never byte-widen it).
std::wstring sandboxWorkspacePathW();

namespace {

// The sandbox capability SID: a random S-1-5-21-<a>-<b>-<c>-<d> (domain-form
// SID used purely as an identity marker, like Codex's cap.rs). Only the sandbox
// workspace ACL grants it access, so only tokens carrying it can write there.
std::string g_capSidStr;
BYTE g_capSidBuf[256];
bool g_capSidInit = false;
std::string g_workspacePath;

// NOTE: a private desktop is NOT usable for the sandbox user. USER32.dll
// initialization requires the window station/desktop ACL to contain the
// process's LogonSession SID, which only winlogon (interactive) logons get;
// CreateProcessWithLogonW's fresh logon session can NEVER initialize USER32
// (0xC0000142) regardless of the desktop ACL. Programs linking USER32 (whoami,
// timeout, ...) simply cannot run inside the sandbox; the script layer must
// use non-USER32 tools (cmd builtins, file ops, python). The helper suppresses
// the "application failed to initialize" dialog via SetErrorMode.
std::string g_privateDesktop;         // unused (documented above)

// Elevated sandbox credentials (Codex elevated backend): the helper runs as a
// dedicated local user, whose password is DPAPI-encrypted (machine scope) and
// stored at <workdir>\.sandbox-secrets\sandbox_user.json. The agent decrypts
// it to LogonUser + CreateRestrictedToken + CreateProcessAsUser.
std::string g_sandboxPassword;
bool g_credsReady = false;

// Serializes the whole sandbox chain (ensure/credentials/execute): concurrent
// callers exist (hook scheduler thread + agent worker threads) and the ensure
// path can launch an elevated setup - without the lock two threads could run
// setup concurrently (double UAC), race the password rotation, or tear the
// global capSid while another thread reads it.
std::mutex g_sandboxMutex;

// Directories granted read+execute to the sandbox user at startup (registered
// from aoi_config.json "sandbox.read_dirs"). Guarded by g_sandboxMutex (set
// once at agent start, read in ensureSandboxReady).

// ---- base64 decode (for the DPAPI blob in the secrets file) ----
bool b64Decode(const std::string& in, std::string* out) { return Base64::Decode(in, out); }

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

// DPAPI (machine-level) decrypt.
std::string dpapiDecrypt(const std::string& b64) {
  std::string blob;
  if (!b64Decode(b64, &blob)) return {};
  DATA_BLOB in{static_cast<DWORD>(blob.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
    return {};
  std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return plain;
}

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  // MultiByteToWideChar writes n wchar_t INCLUDING the null terminator; size
  // the buffer for all n, then drop the terminator (a size-1 buffer would be
  // overflowed when the string's capacity exactly matches).
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.pop_back();
  return w;
}

// Launch aoi-sandbox-setup.exe elevated (UAC). Returns true when setup
// completed successfully.
bool runSandboxSetup() {
  // Wide path from GetModuleFileNameW: the ANSI variant returns GBK bytes on
  // CJK systems and utf8ToWide (which expects UTF-8) would produce mojibake -
  // ShellExecuteExW would fail to find the setup exe, setup would never run,
  // and EVERY later tool call would re-trigger UAC forever ("一直提权").
  wchar_t exeBuf[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) == 0) return false;
  std::wstring dir(exeBuf);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
  const std::wstring setupPath = dir + L"aoi-sandbox-setup.exe";
  if (GetFileAttributesW(setupPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

  // Real user SID (for .sandbox-secrets ACL). Pure ASCII, safe to widen.
  std::string userSidStr;
  {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      DWORD len = 0;
      GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
      std::vector<BYTE> buf(len);
      if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        LPSTR s = nullptr;
        if (ConvertSidToStringSidA(sid, &s)) {
          userSidStr = s;
          LocalFree(s);
        }
      }
      CloseHandle(tok);
    }
  }
  if (userSidStr.empty()) return false;

  const std::wstring workdir = sandboxWorkspacePathW().substr(
      0, sandboxWorkspacePathW().size() - std::wstring(L"sandbox").size());
  const std::wstring sidW = utf8ToWide(userSidStr);
  // ShellExecuteExW appends lpParameters to lpFile to build the command line,
  // so lpParameters must NOT repeat the exe path: "setup setup <workdir> <sid>"
  // would shift setup's argv (workdir=exe path, SID=workdir) and every setup
  // run would fail (secureSecretsDir with a bad SID, err log written to a
  // garbage path) - and each later tool call would re-trigger UAC forever.
  const std::wstring args = L"\"" + workdir + L"\" \"" + sidW + L"\"";

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = setupPath.c_str();
  sei.lpParameters = args.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
  WaitForSingleObject(sei.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(sei.hProcess, &code);
  CloseHandle(sei.hProcess);
  return code == 0;
}

// Load the sandbox user password from .sandbox-secrets; if missing (first
// run) or undecryptable, launch the elevated setup and retry once.
bool ensureSandboxCredentials() {
  if (g_credsReady) return true;
  const std::string workdir = sandboxWorkspacePath().substr(
      0, sandboxWorkspacePath().size() - std::string("sandbox").size());
  const std::string secretsFile = workdir + "\\.sandbox-secrets\\sandbox_user.json";
  std::ifstream f(secretsFile);
  std::string password;
  if (f) {
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    const size_t p = content.find("\"password_b64\":\"");
    if (p != std::string::npos) {
      const size_t start = p + 16;
      const size_t end = content.find('"', start);
      if (end != std::string::npos) password = dpapiDecrypt(content.substr(start, end - start));
    }
  }
  if (password.empty()) {
    // First run or stale credentials: run elevated setup, then reload.
    if (!runSandboxSetup()) return false;
    std::ifstream f2(secretsFile);
    if (!f2) return false;
    std::string content((std::istreambuf_iterator<char>(f2)),
                        std::istreambuf_iterator<char>());
    const size_t p = content.find("\"password_b64\":\"");
    if (p == std::string::npos) return false;
    const size_t start = p + 16;
    const size_t end = content.find('"', start);
    if (end == std::string::npos) return false;
    password = dpapiDecrypt(content.substr(start, end - start));
    if (password.empty()) return false;
  }
  g_sandboxPassword = password;
  g_credsReady = true;
  return true;
}

// Locate a SID with the given attributes inside a TOKEN_GROUPS array.
PSID findSidWithAttributes(const TOKEN_GROUPS* groups, DWORD attr) {
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if (groups->Groups[i].Attributes & attr) return groups->Groups[i].Sid;
  }
  return nullptr;
}

// Resolve the logon SID of the given token into a full-size buffer (SIDs are
// variable-length: sizeof(SID) omits SubAuthority, so fixed structs truncate).
std::vector<BYTE> getLogonSid(HANDLE tok) {
  DWORD len = 0;
  GetTokenInformation(tok, TokenGroups, nullptr, 0, &len);
  std::vector<BYTE> buf(len);
  if (!GetTokenInformation(tok, TokenGroups, buf.data(), len, &len)) return {};
  auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
  PSID logon = findSidWithAttributes(groups, SE_GROUP_LOGON_ID);
  if (!logon) return {};
  std::vector<BYTE> out(GetLengthSid(logon));
  if (!CopySid(static_cast<DWORD>(out.size()), out.data(), logon)) return {};
  return out;
}

// Resolve the sandbox user's SID from a fresh logon token (used for ACLs).
std::vector<BYTE> sandboxUserSid() {
  if (!ensureSandboxCredentials()) return {};
  const std::wstring userW = L"AoiSandboxUser";
  const std::wstring passW = utf8ToWide(g_sandboxPassword);
  HANDLE logon = nullptr;
  if (!LogonUserW(userW.c_str(), L".", passW.c_str(), LOGON32_LOGON_INTERACTIVE,
                  LOGON32_PROVIDER_DEFAULT, &logon))
    return {};
  DWORD len = 0;
  GetTokenInformation(logon, TokenUser, nullptr, 0, &len);
  std::vector<BYTE> buf(len);
  std::vector<BYTE> out;
  if (GetTokenInformation(logon, TokenUser, buf.data(), len, &len)) {
    PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    out.resize(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(out.size()), out.data(), sid)) out.clear();
  }
  CloseHandle(logon);
  return out;
}

// Ensure the sandbox workspace directory exists.
void ensureWorkspaceDir() {
  g_workspacePath = sandboxWorkspacePath();
  std::error_code ec;
  std::filesystem::create_directories(g_workspacePath, ec);
}

// <agent exe dir>/_capsid.txt (workdir, not the sandbox-writable workspace).
std::string capSidFile() {
  const std::string ws = sandboxWorkspacePath();
  const size_t slash = ws.find_last_of("\\/");
  const std::string dir = slash != std::string::npos ? ws.substr(0, slash + 1) : "";
  return dir + "_capsid.txt";
}

// Generate (once) the capability SID and persist it NEXT TO the sandbox dir
// (the workdir, read-only for the sandbox user): the workspace itself is
// writable by the model, and a persisted SID inside it could be tampered with
// (self-escaped write grants / restricting-SID poisoning).
void initCapabilitySid() {
  if (g_capSidInit) return;
  std::mt19937_64 rng(static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count()) ^
      reinterpret_cast<uintptr_t>(&rng));
  const uint32_t a = static_cast<uint32_t>(rng());
  const uint32_t b = static_cast<uint32_t>(rng());
  const uint32_t c = static_cast<uint32_t>(rng());
  const uint32_t d = static_cast<uint32_t>(rng());
  char buf[128];
  snprintf(buf, sizeof(buf), "S-1-5-21-%u-%u-%u-%u", a, b, c, d);
  g_capSidStr = buf;
  // Persist for reuse across runs (workdir: sandbox user is read-only there).
  const std::string file = capSidFile();
  std::ofstream f(file, std::ios::binary);
  if (f) f << g_capSidStr;
  PSID psid = nullptr;
  if (ConvertStringSidToSidA(g_capSidStr.c_str(), &psid)) {
    CopySid(sizeof(g_capSidBuf), g_capSidBuf, psid);
    LocalFree(psid);
    g_capSidInit = true;
  }
}

// Load a previously persisted capability SID, if any. The loaded SID is
// validated strictly: it must be a domain-form S-1-5-21-<a>-<b>-<c>-<d>
// (identifier authority 5 / subauthority 21 with EXACTLY 4 trailing
// subauthorities). Anything else (e.g. a tampered S-1-5-11 = Authenticated
// Users) would turn the workspace ACL grant (and, if ever used, a restricting
// SID) into a machine-wide write grant - the capability SID must never be
// chosen by the sandbox user.
bool loadPersistedCapSid() {
  const std::string file = capSidFile();
  std::ifstream f(file);
  if (!f) return false;
  std::string line;
  std::getline(f, line);
  if (line.empty()) return false;
  // Strict shape check: S-1-5-21-<uint>-<uint>-<uint>-<uint>
  static const char kPrefix[] = "S-1-5-21-";
  if (line.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
  std::vector<std::string> parts;
  std::string cur;
  const std::string body = line.substr(sizeof(kPrefix) - 1);
  for (size_t i = 0; i <= body.size(); ++i) {
    if (i == body.size() || body[i] == '-') {
      if (cur.empty()) return false;
      parts.push_back(cur);
      cur.clear();
    } else {
      if (body[i] < '0' || body[i] > '9') return false;
      cur += body[i];
    }
  }
  if (parts.size() != 4) return false;
  PSID psid = nullptr;
  if (!ConvertStringSidToSidA(line.c_str(), &psid)) return false;
  CopySid(sizeof(g_capSidBuf), g_capSidBuf, psid);
  LocalFree(psid);
  g_capSidStr = line;
  g_capSidInit = true;
  return true;
}

// Install an ACE on a directory's DACL (idempotent). mode: SET_ACCESS /
// DENY_ACCESS. mask: access mask. sid: the target SID.
bool applyAceToPath(const std::string& path, DWORD mode, DWORD mask, PSID sid) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  DWORD err = GetNamedSecurityInfoA(path.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                    &dacl, nullptr, &sd);
  if (err != ERROR_SUCCESS) return false;
  if (dacl) {
    // Idempotence: skip if an identical ACE already exists.
    ACL_SIZE_INFORMATION info{};
    if (GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
      for (DWORD i = 0; i < info.AceCount; ++i) {
        ACE_HEADER* hdr = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<LPVOID*>(&hdr))) continue;
        if (hdr->AceType == (mode == SET_ACCESS ? ACCESS_ALLOWED_ACE_TYPE
                                                : ACCESS_DENIED_ACE_TYPE)) {
          auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(hdr);
          if (ace->Mask == mask && EqualSid(&ace->SidStart, sid)) {
            LocalFree(sd);
            return true;
          }
        }
      }
    }
  }
  EXPLICIT_ACCESS ea{};
  ea.grfAccessMode = static_cast<ACCESS_MODE>(mode);
  ea.grfAccessPermissions = mask;
  ea.grfInheritance = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.ptstrName = reinterpret_cast<LPCH>(sid);
  PACL newDacl = nullptr;
  err = SetEntriesInAclA(1, &ea, dacl, &newDacl);
  if (err == ERROR_SUCCESS && newDacl) {
    err = SetNamedSecurityInfoA(const_cast<char*>(path.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                newDacl, nullptr);
    LocalFree(newDacl);
  }
  LocalFree(sd);
  return err == ERROR_SUCCESS;
}

// Protect the agent workdir from the sandbox user: disable DACL inheritance
// (the parent chain commonly grants Authenticated Users Modify on D: drives,
// which the sandbox user inherits - a sandbox escape) and rebuild the DACL as
// SYSTEM/Administrators/owner full + Authenticated Users/Users read-only.
// The sandbox workspace keeps its explicit allow ACE, so writes there still
// work (allow rights accumulate; no deny involved).
bool protectWorkdir(const std::string& workdir) {
  // Well-known SIDs are variable-length (Administrators has sub-authorities):
  // fixed SID structs truncate them, so use full-size buffers.
  BYTE sysBuf[68]{}, adminsBuf[68]{};
  {
    DWORD l1 = sizeof(sysBuf), l2 = sizeof(adminsBuf);
    // A failed CreateWellKnownSid leaves a zeroed SID; feeding that into the
    // ACL would corrupt the DACL. Fail closed instead.
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr,
                            reinterpret_cast<PSID>(sysBuf), &l1) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            reinterpret_cast<PSID>(adminsBuf), &l2)) {
      return false;
    }
  }
  PSID authUsers = nullptr;
  PSID users = nullptr;
  ConvertStringSidToSidA("S-1-5-11", &authUsers);   // Authenticated Users
  ConvertStringSidToSidA("S-1-5-32-545", &users);   // BUILTIN\Users

  EXPLICIT_ACCESS eas[5]{};
  int n = 0;
  auto addEa = [&](PSID sid, DWORD mask) {
    eas[n].grfAccessMode = GRANT_ACCESS;
    eas[n].grfAccessPermissions = mask;
    eas[n].grfInheritance = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    eas[n].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    eas[n].Trustee.ptstrName = reinterpret_cast<LPCH>(sid);
    ++n;
  };
  addEa(reinterpret_cast<PSID>(sysBuf), FILE_ALL_ACCESS);
  addEa(reinterpret_cast<PSID>(adminsBuf), FILE_ALL_ACCESS);
  if (authUsers) addEa(authUsers, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  if (users) addEa(users, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  // The owner (the real user) keeps full access implicitly as owner; also
  // grant explicitly for robustness.
  HANDLE tok = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
    DWORD len = 0;
    GetTokenInformation(tok, TokenOwner, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    if (GetTokenInformation(tok, TokenOwner, buf.data(), len, &len)) {
      PSID owner = reinterpret_cast<TOKEN_OWNER*>(buf.data())->Owner;
      addEa(owner, FILE_ALL_ACCESS);
    }
    CloseHandle(tok);
  }

  PACL newDacl = nullptr;
  const DWORD err = SetEntriesInAclA(n, eas, nullptr, &newDacl);
  if (err != ERROR_SUCCESS || !newDacl) {
    if (authUsers) LocalFree(authUsers);
    if (users) LocalFree(users);
    return false;
  }
  const DWORD setErr = SetNamedSecurityInfoA(
      const_cast<char*>(workdir.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, newDacl, nullptr);
  LocalFree(newDacl);
  if (authUsers) LocalFree(authUsers);
  if (users) LocalFree(users);
  return setErr == ERROR_SUCCESS;
}

// One-time sandbox setup: workspace dir + capability SID + ACL. Also denies
// the sandbox user access to .sandbox-secrets (its DPAPI blob is machine-
// scoped, so a readable ciphertext would be decryptable by the sandbox user).
void ensureSandboxReady() {
  ensureWorkspaceDir();
  if (!loadPersistedCapSid()) initCapabilitySid();
  // ACL: grant the capability SID full access to the workspace (read+write+
  // execute+delete, inherited). Everything outside stays un-granted, so the
  // WRITE_RESTRICTED token cannot write there. Every ACL step is CHECKED: a
  // silently failing DENY/protect would leave the sandbox credentials,
  // AGENTS.md or system skills writable by the model (self-escape).
  if (!applyAceToPath(g_workspacePath, SET_ACCESS,
                      FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE |
                          DELETE,
                      reinterpret_cast<PSID>(g_capSidBuf)))
    std::fprintf(stderr, "[Sandbox] WARNING: applyAceToPath(workspace capSID) failed, err=%lu\n", GetLastError());
  // NUL device must remain usable for redirection.
  applyAceToPath("\\\\.\\NUL", SET_ACCESS,
                 FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE |
                     DELETE,
                 reinterpret_cast<PSID>(g_capSidBuf));
  // .sandbox-secrets ACL (SYSTEM/Admins/real user full + sandbox user DENY) is
  // set by the ELEVATED setup: the agent has no WRITE_DAC on the directory
  // (only inherited Authenticated Users Modify), so attempts here would fail
  // silently and leave the machine-level DPAPI password readable by the
  // sandbox user. AGENTS.md and the system skills root are ours to secure:
  if (ensureSandboxCredentials()) {
    std::vector<BYTE> userSid = sandboxUserSid();
    if (!userSid.empty()) {
      const std::string workdir = g_workspacePath.substr(
          0, g_workspacePath.size() - std::string("sandbox").size());
      // AGENTS.md (user-editable extra system prompt): the sandbox user must
      // NOT be able to create/modify it - otherwise the model could inject
      // its own instructions. Ensure it exists (agent-created, user-writable)
      // and deny the sandbox user all access.
      const std::string agentMd = g_workspacePath + "\\AGENTS.md";
      { std::ofstream touch(agentMd, std::ios::app); }
      if (!applyAceToPath(agentMd, DENY_ACCESS, FILE_ALL_ACCESS,
                          reinterpret_cast<PSID>(userSid.data())))
        std::fprintf(stderr, "[Sandbox] WARNING: DENY AGENTS.md failed, err=%lu\n", GetLastError());
      // System skills root (sandbox/skills): the sandbox user must NOT be able
      // to create/modify system-provided skills (their catalog is injected
      // into the model's context) - same protection as AGENTS.md. User skills
      // live in sandbox/skills_user and stay writable.
      // NOTE: DENY WRITE ONLY - the model must be able to READ SKILL.md files
      // (the catalog tells it to read skills/<name>/SKILL.md). FILE_ALL_ACCESS
      // denied reads too (observed: model's read of skills/vrchat-assistant/
      // SKILL.md failed with "file not found" every time, so it never learned
      // the workflow and hallucinated numbers).
      const std::string sysSkills = g_workspacePath + "\\skills";
      { std::error_code ec; std::filesystem::create_directories(sysSkills, ec); }
      // Precise write-only bits: FILE_GENERIC_WRITE expands to include
      // SYNCHRONIZE, which is REQUIRED to open a file at all - a DENY on it
      // blocks reads too (observed: model's read of SKILL.md failed despite
      // the allow ACE). Keep reads (FILE_READ_DATA/ATTRIBUTES) out of the
      // deny mask.
      constexpr DWORD kSkillDenyMask =
          FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
          FILE_WRITE_ATTRIBUTES | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD |
          DELETE | WRITE_DAC | WRITE_OWNER;
      if (!applyAceToPath(sysSkills, DENY_ACCESS, kSkillDenyMask,
                          reinterpret_cast<PSID>(userSid.data())))
        std::fprintf(stderr, "[Sandbox] WARNING: DENY sandbox/skills failed, err=%lu\n", GetLastError());
      // VRCX companion database (VRChat auth session store used by the
      // sql_query tool) lives under the REAL user's profile (%APPDATA%\VRCX),
      // whose ACL grants no read to the sandbox user by default - sqlite3
      // would fail with "unable to open database file". Grant the sandbox
      // user READ+EXECUTE on the VRCX directory (inherited by the db file),
      // mirroring how Codex opens AppData to its CodexSandboxUsers group.
      // The agent runs as the real user, so this needs no elevation.
      {
        const char* appdata = std::getenv("APPDATA");
        if (appdata && *appdata) {
          const std::string vrcx = std::string(appdata) + "\\VRCX";
          if (GetFileAttributesA(vrcx.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!applyAceToPath(vrcx, SET_ACCESS,
                                FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
                                reinterpret_cast<PSID>(userSid.data())))
              std::fprintf(stderr, "[Sandbox] WARNING: VRCX read grant failed, err=%lu\n",
                           GetLastError());
          }
        }
      }
      // The sandbox workspace must be writable by the sandbox user: the
      // workdir protection above rebuilds the parent ACL as read-only for
      // Users/Authenticated Users, and new files in sandbox/ would inherit
      // that read-only ACE. Add an explicit allow for the sandbox user so
      // new files (scripts, outputs) are writable.
      if (!applyAceToPath(g_workspacePath, SET_ACCESS,
                          FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                              FILE_GENERIC_EXECUTE | DELETE,
                          reinterpret_cast<PSID>(userSid.data())))
        std::fprintf(stderr, "[Sandbox] WARNING: applyAceToPath(workspace user allow) failed, err=%lu\n", GetLastError());
      // Workdir write protection: the sandbox user inherits Authenticated
      // Users / Users modify rights on the agent directory (common on D:
      // drives), which would let the model write anywhere next to the agent
      // (aoi_config.json, history.json, the exe...). Disable DACL inheritance
      // on the workdir and rebuild it read-only for those groups; the sandbox
      // workspace keeps its explicit allow so writes there still work.
      if (!protectWorkdir(workdir))
        std::fprintf(stderr, "[Sandbox] WARNING: protectWorkdir failed, err=%lu\n", GetLastError());
    }
  }
}

// Create the WRITE_RESTRICTED token (Codex elevated backend):
// LogonUser as the dedicated sandbox user, then CreateRestrictedToken on that
// login token with DISABLE_MAX_PRIVILEGE | LUA_TOKEN | WRITE_RESTRICTED.
// Restricting SIDs = [capability SID, sandbox user SID, logon SID, Everyone].
// The process identity is the sandbox user (no access outside the workspace)
// AND the token is write-restricted (capability-SID-gated) - double isolation.
bool createRestrictedToken(HANDLE* outToken) {
  ensureSandboxReady();
  if (!g_capSidInit) return false;

  // 1) Log on as the sandbox user (interactive logon, primary token). On
  // failure (stale/rotated password) re-run the elevated setup (which rotates
  // the password) and retry once - Codex's credential self-heal.
  const std::wstring userW = L"AoiSandboxUser";
  const std::wstring passW = utf8ToWide(g_sandboxPassword);
  const std::wstring domainW = L".";
  HANDLE logon = nullptr;
  if (!LogonUserW(userW.c_str(), domainW.c_str(), passW.c_str(),
                  LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &logon)) {
    // Stale/rotated password: mark creds dirty so ensureSandboxCredentials
    // re-runs the elevated setup (which rotates the password and rewrites
    // .sandbox-secrets), then retry once with the fresh password. Retrying
    // with the SAME password could never succeed.
    g_credsReady = false;
    if (!ensureSandboxCredentials()) return false;
    const std::wstring passW2 = utf8ToWide(g_sandboxPassword);
    if (!LogonUserW(userW.c_str(), domainW.c_str(), passW2.c_str(),
                    LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT,
                    &logon)) {
      return false;
    }
  }

  // 2) Restrict the login token.
  SID worldSid{};
  {
    DWORD len = sizeof(worldSid);
    CreateWellKnownSid(WinWorldSid, nullptr, &worldSid, &len);
  }
  std::vector<BYTE> logonSid = getLogonSid(logon);

  // Sandbox user SID (identity marker in the restricting list).
  std::vector<BYTE> userSid;
  {
    DWORD len = 0;
    GetTokenInformation(logon, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    if (GetTokenInformation(logon, TokenUser, buf.data(), len, &len)) {
      PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
      userSid.resize(GetLengthSid(sid));
      if (!CopySid(static_cast<DWORD>(userSid.size()), userSid.data(), sid))
        userSid.clear();
    }
  }

  std::vector<SID_AND_ATTRIBUTES> restricting;
  restricting.push_back({reinterpret_cast<PSID>(g_capSidBuf), 0});
  if (!userSid.empty()) restricting.push_back({reinterpret_cast<PSID>(userSid.data()), 0});
  if (!logonSid.empty()) restricting.push_back({reinterpret_cast<PSID>(logonSid.data()), 0});
  restricting.push_back({&worldSid, 0});

  HANDLE restricted = nullptr;
  const BOOL ok = CreateRestrictedToken(
      logon, DISABLE_MAX_PRIVILEGE | LUA_TOKEN | WRITE_RESTRICTED, 0, nullptr,
      0, nullptr, static_cast<DWORD>(restricting.size()), restricting.data(),
      &restricted);
  CloseHandle(logon);
  if (!ok || !restricted) return false;

  // Default DACL: grant logon SID + Everyone + capability SID so the sandboxed
  // process can create named pipes / IPC objects (Codex token.rs).
  {
    EXPLICIT_ACCESS eas[3]{};
    int n = 0;
    auto addEa = [&](PSID sid) {
      if (!sid) return;
      eas[n].grfAccessMode = GRANT_ACCESS;
      eas[n].grfAccessPermissions = GENERIC_ALL;
      eas[n].grfInheritance = NO_INHERITANCE;
      eas[n].Trustee.TrusteeForm = TRUSTEE_IS_SID;
      eas[n].Trustee.ptstrName = reinterpret_cast<LPCH>(sid);
      ++n;
    };
    addEa(logonSid.empty() ? nullptr : reinterpret_cast<PSID>(logonSid.data()));
    addEa(&worldSid);
    addEa(reinterpret_cast<PSID>(g_capSidBuf));
    PACL dacl = nullptr;
    if (SetEntriesInAclA(n, eas, nullptr, &dacl) == ERROR_SUCCESS && dacl) {
      SetTokenInformation(restricted, TokenDefaultDacl, &dacl, sizeof(PACL));
      LocalFree(dacl);
    }
  }

  // Re-enable SeChangeNotifyPrivilege (directory traversal) - Codex token.rs.
  {
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(nullptr, "SeChangeNotifyPrivilege",
                              &tp.Privileges[0].Luid)) {
      AdjustTokenPrivileges(restricted, FALSE, &tp, 0, nullptr, nullptr);
    }
  }

  *outToken = restricted;
  return true;
}

} // namespace



// Wide-char variants used by the CreateProcessWithLogonW path: the ANSI
// module path is GBK on CJK systems and must never be byte-widened.
std::wstring sandboxWorkspacePathW() {
  wchar_t buf[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return L"";
  std::wstring dir(buf);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
  return dir + L"sandbox";
}

std::wstring findHelperPathW() {
  wchar_t buf[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
    std::wstring dir(buf);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
    const std::wstring p = dir + L"aoi-sandbox-helper.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return L"aoi-sandbox-helper.exe";  // rely on PATH
}

std::string sandboxWorkspacePath() {
  // <agent exe dir>/sandbox
  char buf[MAX_PATH]{};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string dir;
  if (n > 0) {
    dir = buf;
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash + 1);
  }
  return dir + "sandbox";
}

std::string sandboxExecuteInner(const std::string& opJson);

std::string sandboxExecute(const std::string& opJson) {
  try {
    return sandboxExecuteInner(opJson);
  } catch (const std::exception& ex) {
    return std::string("{\"ok\":false,\"error\":\"(sandbox exception: ") + ex.what() + "\"}";
  } catch (...) {
    return R"json({"ok":false,"error":"(sandbox: unknown exception)"})json";
  }
}

std::string sandboxExecuteInner(const std::string& opJson) {
  std::lock_guard<std::mutex> lk(g_sandboxMutex);
  ensureSandboxReady();
  if (!ensureSandboxCredentials()) {
    return R"json({"ok":false,"error":"(sandbox: credentials unavailable - refusing to execute)"})json";
  }

  const std::wstring helperW = findHelperPathW();
  if (helperW.empty() ||
      GetFileAttributesW(helperW.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return R"json({"ok":false,"error":"(sandbox: aoi-sandbox-helper.exe not found - refusing to execute)"})json";
  }

  // Anonymous pipes for stdin/stdout.
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
  if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) {
    // Short-circuit: if the FIRST pipe succeeded but the second failed, its
    // handles leak (this path repeats on every sandboxed op).
    if (inRead) CloseHandle(inRead);
    if (inWrite) CloseHandle(inWrite);
    if (outRead) CloseHandle(outRead);
    if (outWrite) CloseHandle(outWrite);
    return R"json({"ok":false,"error":"(sandbox: CreatePipe failed)"})json";
  }
  SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

  // Job object: kill the whole tree on timeout.
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li,
                            sizeof(li));
  }

  // Command line: helper.exe "<sandbox workspace>" "<capability SID>" (cwd for
  // the helper; the op JSON travels on stdin - no quoting issues). The capSID
  // lets the helper build the WRITE_RESTRICTED token for bash children (Codex
  // elevated-backend semantics). Built as wide strings from GetModuleFileNameW:
  // the ANSI variant returns GBK bytes on CJK systems and a byte-wise widen
  // produces mojibake (CreateProcessWithLogonW would fail on any non-ASCII
  // install path).
  const std::wstring wsSandboxW = sandboxWorkspacePathW();
  // exeDir = workspace minus the trailing "sandbox" (7 chars). CRITICAL: the
  // result ends with a backslash, and a trailing "\" inside a quoted command-
  // line argument is parsed by CommandLineToArgvW as an ESCAPED QUOTE - the
  // helper's argv[3] comes out wrong (e.g. `Release"`), the exeDir fallback
  // for read resolves to a broken path, and reading shipped files
  // (docs/..., SKILL.md) silently fails. Strip the trailing separator.
  std::wstring exeDirW =
      wsSandboxW.substr(0, wsSandboxW.size() - std::wstring(L"sandbox").size());
  while (!exeDirW.empty() &&
         (exeDirW.back() == L'\\' || exeDirW.back() == L'/'))
    exeDirW.pop_back();
  std::wstring cmdLine = L"\"" + helperW + L"\" \"" + wsSandboxW + L"\" \"" +
                         utf8ToWide(g_capSidStr) + L"\" \"" + exeDirW + L"\"";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = outWrite;

  // The sandbox user cannot open the interactive desktop (winsta0\default is
  // scoped to the logged-in user's session), so USER32-dependent programs fail
  // DLL initialization (0xC0000142). Use the private desktop created for the
  // sandbox user instead.
  const std::wstring desktopW = utf8ToWide(g_privateDesktop);
  si.lpDesktop = g_privateDesktop.empty()
                     ? const_cast<LPWSTR>(L"winsta0\\default")
                     : const_cast<LPWSTR>(desktopW.c_str());

  // CreateProcessWithLogonW needs NO special privileges (unlike
  // CreateProcessAsUserW/WithTokenW which require SeAssignPrimaryToken /
  // SeImpersonate - absent on some hardened systems) and performs the logon
  // itself; the helper then runs as the dedicated sandbox user. Flags 0 =
  // do not load the sandbox user's profile.
  const std::wstring userW = L"AoiSandboxUser";
  const std::wstring passW = utf8ToWide(g_sandboxPassword);
  PROCESS_INFORMATION pi{};
  // LOGON_WITH_PROFILE: the sandbox user has no profile directory; without it
  // USERPROFILE etc. point at a non-existent C:\Users\AoiSandboxUser and
  // console programs (whoami, ...) fail DLL initialization with 0xC0000142.
  // Loading the profile fixes the user environment (first run creates it).
  BOOL ok = CreateProcessWithLogonW(userW.c_str(), L".", passW.c_str(),
                                    LOGON_WITH_PROFILE, nullptr, cmdLine.data(),
                                    CREATE_UNICODE_ENVIRONMENT |
                                        CREATE_NO_WINDOW,
                                    nullptr, wsSandboxW.c_str(), &si, &pi);
  if (!ok) {
    CloseHandle(inRead);
    CloseHandle(inWrite);
    CloseHandle(outRead);
    CloseHandle(outWrite);
    if (job) CloseHandle(job);
    return R"json({"ok":false,"error":"(sandbox: CreateProcessAsUser failed - refusing to execute)"})json";
  }

  CloseHandle(inRead);
  CloseHandle(outWrite);
  if (job) AssignProcessToJobObject(job, pi.hProcess);

  // Send the op JSON on stdin, close it (helper reads stdin to EOF). Loop so
  // a partial write (pipe backpressure) never truncates the op mid-JSON.
  {
    const char* p = opJson.data();
    size_t remaining = opJson.size();
    while (remaining > 0) {
      DWORD written = 0;
      if (!WriteFile(inWrite, p, static_cast<DWORD>(remaining), &written, nullptr) ||
          written == 0) {
        break;
      }
      p += written;
      remaining -= written;
    }
  }
  CloseHandle(inWrite);

  // Read helper output (bounded by timeout).
  std::string out;
  char buf[4096];
  DWORD n = 0;
  const DWORD kTimeoutMs = 30000;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    // Non-blocking-ish: poll with small sleeps.
    DWORD avail = 0;
    if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      if (!ReadFile(outRead, buf, sizeof(buf), &n, nullptr) || n == 0) break;
      out.append(buf, n);
      if (out.size() > 65536) break;
    } else {
      DWORD code = 0;
      if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    if (job) TerminateJobObject(job, 1);
    else TerminateProcess(pi.hProcess, 1);
  }
  // Drain any remaining output.
  while (PeekNamedPipe(outRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
    if (!ReadFile(outRead, buf, sizeof(buf), &n, nullptr) || n == 0) break;
    out.append(buf, n);
    if (out.size() > 65536) break;
  }

  // Read the exit code BEFORE closing the process handle (reading it after
  // CloseHandle(pi.hProcess) returns garbage - observed as a fake "exit=0"
  // that masked a real 0xC0000409 crash).
  DWORD helperExit = 0;
  GetExitCodeProcess(pi.hProcess, &helperExit);

  CloseHandle(outRead);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);

  if (out.empty()) {
    return R"json({"ok":false,"error":"(sandbox: helper produced no output, exit=)json" +
           std::to_string(helperExit) + R"json()})json";
  }
  return out;
}

} // namespace aoi
