// aoi-sandbox-setup: one-time elevated configuration for the Aoi agent
// sandbox (aligned with Codex's elevated Windows backend). Runs once with
// administrator rights (launched via ShellExecuteExW "runas" by the agent):
//
//   1. Create the local sandbox user + group:
//        user : AoiSandboxUser   (password: random 24 chars, rotated on rerun)
//        group: AoiSandboxUsers
//   2. Encrypt the password with machine-level DPAPI and store it at
//      <workdir>\.sandbox-secrets\sandbox_user.json
//   3. Hide the user from the Windows login screen (Winlogon UserList)
//   4. Secure .sandbox-secrets (SYSTEM/Administrators/<real user> full,
//      AoiSandboxUsers DENY) so the sandbox user can never read the password
//   5. Grant AoiSandboxUsers full access to <workdir>\sandbox (the ONLY
//      writable root - the sandbox user has no rights anywhere else)
//
// Usage: aoi-sandbox-setup.exe <workdir> <realUserSid>
// Exit code 0 on success, nonzero on failure.
#include <windows.h>

#include <aclapi.h>
#include <lm.h>
#include <ntsecapi.h>
#include <userenv.h>
#include <ntstatus.h>
#include <sddl.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

const wchar_t kUserName[] = L"AoiSandboxUser";
const wchar_t kGroupName[] = L"AoiSandboxUsers";
const char kSecretsDir[] = ".sandbox-secrets";
const char kSecretsFile[] = "sandbox_user.json";
const char kSandboxDir[] = "sandbox";

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  // Buffer for all n bytes INCLUDING the null terminator, then drop it.
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  s.pop_back();
  return s;
}

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  // Buffer for all n wchar_t INCLUDING the null terminator, then drop it.
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.pop_back();
  return w;
}

// Random 24-char password (letters/digits/symbols).
std::string randomPassword() {
  static const char chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*";
  std::mt19937_64 rng(std::random_device{}());
  std::string pwd;
  pwd.reserve(24);
  for (int i = 0; i < 24; ++i) pwd += chars[rng() % (sizeof(chars) - 1)];
  return pwd;
}

// DPAPI (machine-level) encrypt -> base64 string.
std::string dpapiEncrypt(const std::string& plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"AoiSandboxUser", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                        &out))
    return {};
  std::string b64;
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (DWORD i = 0; i < out.cbData; i += 3) {
    const unsigned b0 = out.pbData[i];
    const unsigned b1 = i + 1 < out.cbData ? out.pbData[i + 1] : 0;
    const unsigned b2 = i + 2 < out.cbData ? out.pbData[i + 2] : 0;
    b64 += kAlphabet[b0 >> 2];
    b64 += kAlphabet[((b0 & 3) << 4) | (b1 >> 4)];
    b64 += i + 1 < out.cbData ? kAlphabet[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    b64 += i + 2 < out.cbData ? kAlphabet[b2 & 63] : '=';
  }
  LocalFree(out.pbData);
  return b64;
}

// base64 decode (RFC 4648, standard alphabet, tolerant of padding).
std::string b64Decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int acc = 0, bits = 0;
  for (const char c : in) {
    if (c == '=' || c == '\r' || c == '\n') continue;
    const int v = val(c);
    if (v < 0) return {};
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

// DPAPI (machine-level) decrypt.
std::string dpapiDecrypt(const std::string& b64) {
  const std::string blob = b64Decode(b64);
  if (blob.empty()) return {};
  DATA_BLOB in{static_cast<DWORD>(blob.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
    return {};
  std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return plain;
}

// Read the existing secrets file and decrypt the stored password ("" when
// absent/undecryptable) - reused across setup reruns so packages sharing the
// sandbox user never invalidate each other.
std::string existingPassword(const std::string& workdir) {
  const std::string file = workdir + "\\" + kSecretsDir + "\\" + kSecretsFile;
  std::ifstream f(file, std::ios::binary);
  if (!f) return {};
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  const size_t p = content.find("\"password_b64\":\"");
  if (p == std::string::npos) return {};
  const size_t start = p + 16;
  const size_t end = content.find('"', start);
  if (end == std::string::npos) return {};
  return dpapiDecrypt(content.substr(start, end - start));
}

// True when the password still logs the sandbox user in.
bool passwordValid(const std::string& password) {
  if (password.empty()) return false;
  const std::wstring passW = utf8ToWide(password);
  HANDLE logon = nullptr;
  const BOOL ok = LogonUserW(kUserName, L".", passW.c_str(),
                             LOGON32_LOGON_INTERACTIVE,
                             LOGON32_PROVIDER_DEFAULT, &logon);
  if (ok) CloseHandle(logon);
  return ok != FALSE;
}

// Create the sandbox user (idempotent) and add it to the Users group (base
// compatibility - full-disk read requires it). When `setPassword` is false
// and the user already exists, the password is NOT rotated: several packages
// share the single sandbox user, and rotating on every setup run would
// invalidate the other packages' secrets files ("一直提权" loop).
bool provisionUsers(const std::string& password, bool setPassword) {
  const std::wstring passW = utf8ToWide(password);
  USER_INFO_1 ui1{};
  ui1.usri1_name = const_cast<LPWSTR>(kUserName);
  ui1.usri1_password = const_cast<LPWSTR>(passW.c_str());
  ui1.usri1_priv = USER_PRIV_USER;
  ui1.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
  ui1.usri1_script_path = nullptr;
  NET_API_STATUS rc = NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&ui1), nullptr);
  if (rc == NERR_UserExists) {
    if (setPassword) {
      USER_INFO_1003 ui1003{};
      ui1003.usri1003_password = ui1.usri1_password;
      rc = NetUserSetInfo(nullptr, kUserName, 1003,
                          reinterpret_cast<LPBYTE>(&ui1003), nullptr);
      if (rc != NERR_Success) return false;
    }
  } else if (rc != NERR_Success) {
    return false;
  }

  LOCALGROUP_MEMBERS_INFO_3 mi3{};
  mi3.lgrmi3_domainandname = const_cast<LPWSTR>(kUserName);
  rc = NetLocalGroupAddMembers(nullptr, L"Users", 3,
                               reinterpret_cast<LPBYTE>(&mi3), 1);
  return rc == NERR_Success || rc == ERROR_MEMBER_IN_ALIAS;
}

// Apply an ACE to a directory DACL (idempotent).
bool applyDirAce(const std::wstring& path, DWORD mode, DWORD mask, PSID sid) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  const std::string p = wideToUtf8(path);
  DWORD err = GetNamedSecurityInfoA(p.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                    &dacl, nullptr, &sd);
  if (err != ERROR_SUCCESS) return false;
  if (dacl) {
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
    err = SetNamedSecurityInfoA(const_cast<char*>(p.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                newDacl, nullptr);
    LocalFree(newDacl);
  }
  LocalFree(sd);
  return err == ERROR_SUCCESS;
}

// Convert a SID string to a heap-allocated PSID (LocalFree to release).
PSID sidFromString(const char* str) {
  PSID sid = nullptr;
  if (!ConvertStringSidToSidA(str, &sid)) return nullptr;
  return sid;
}

// Grant the sandbox user the two privileges the restricted-token spawn needs
// (aligned with Codex: the elevated runner runs AS the sandbox user and calls
// CreateProcessAsUserW, which requires SeAssignPrimaryToken +
// SeIncreaseQuota on the CALLING process's token). LsaAddAccountRights is
// set-semantics: re-running setup converges to exactly this right set, no
// duplicates, and removing this call later revokes the rights.
bool grantSandboxPrivileges() {
  LSA_HANDLE policy = nullptr;
  LSA_OBJECT_ATTRIBUTES oa{};
  NTSTATUS st = LsaOpenPolicy(nullptr, &oa,
                              POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES |
                                  POLICY_VIEW_LOCAL_INFORMATION,
                              &policy);
  if (st != STATUS_SUCCESS) return false;

  SID_NAME_USE use{};
  wchar_t domain[256]{};
  DWORD domLen = 256;
  BYTE sidBuf[SECURITY_MAX_SID_SIZE]{};
  DWORD sidLen = sizeof(sidBuf);
  if (!LookupAccountNameW(nullptr, kUserName, sidBuf, &sidLen, domain, &domLen,
                          &use)) {
    LsaClose(policy);
    return false;
  }

  static const wchar_t* kRightNames[] = {L"SeAssignPrimaryTokenPrivilege",
                                         L"SeIncreaseQuotaPrivilege"};
  LSA_UNICODE_STRING rights[2]{};
  for (int i = 0; i < 2; ++i) {
    rights[i].Buffer = const_cast<LPWSTR>(kRightNames[i]);
    rights[i].Length =
        static_cast<USHORT>(wcslen(kRightNames[i]) * sizeof(wchar_t));
    rights[i].MaximumLength = rights[i].Length + sizeof(wchar_t);
  }
  st = LsaAddAccountRights(policy, reinterpret_cast<PSID>(sidBuf), rights, 2);
  LsaClose(policy);
  return st == STATUS_SUCCESS;
}

// Secure the .sandbox-secrets directory: disable DACL inheritance and grant
// SYSTEM / Administrators / the real user full access; DENY the sandbox user
// everything. Must run ELEVATED: the non-elevated agent has no WRITE_DAC on
// the directory (it only inherits Authenticated Users Modify), so its own
// attempts to add the DENY ACE silently failed - the sandbox user could read
// (and decrypt, machine-level DPAPI) the password blob.
bool secureSecretsDir(const std::string& workdir, const std::string& realUserSid,
                      std::string& errOut) {
  const std::string dir = workdir + "\\" + kSecretsDir;

  PSID sysSid = nullptr, adminsSid = nullptr, realSid = nullptr,
       sandboxSid = nullptr;
  const DWORD maxSid = SECURITY_MAX_SID_SIZE;
  BYTE sysBuf[SECURITY_MAX_SID_SIZE]{}, adminsBuf[SECURITY_MAX_SID_SIZE]{};
  {
    DWORD len = maxSid;
    CreateWellKnownSid(WinLocalSystemSid, nullptr, sysBuf, &len);
    len = maxSid;
    CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminsBuf, &len);
  }
  SID_NAME_USE use{};
  BYTE sandboxBuf[SECURITY_MAX_SID_SIZE]{};
  {
    wchar_t domain[256]{};
    DWORD domLen = 256, len = maxSid;
    LookupAccountNameW(nullptr, kUserName, sandboxBuf, &len, domain, &domLen,
                       &use);
  }
  sysSid = reinterpret_cast<PSID>(sysBuf);
  adminsSid = reinterpret_cast<PSID>(adminsBuf);
  sandboxSid = reinterpret_cast<PSID>(sandboxBuf);
  realSid = sidFromString(realUserSid.c_str());
  if (!realSid) {
    errOut = "secureSecretsDir: bad real user SID";
    return false;
  }

  EXPLICIT_ACCESS eas[4]{};
  int n = 0;
  auto addEa = [&](PSID sid, DWORD mask, ACCESS_MODE mode) {
    eas[n].grfAccessMode = mode;
    eas[n].grfAccessPermissions = mask;
    eas[n].grfInheritance = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    eas[n].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    eas[n].Trustee.ptstrName = reinterpret_cast<LPCH>(sid);
    ++n;
  };
  const DWORD full = GENERIC_ALL;
  addEa(sysSid, full, GRANT_ACCESS);
  addEa(adminsSid, full, GRANT_ACCESS);
  addEa(realSid, full, GRANT_ACCESS);
  addEa(sandboxSid, full, DENY_ACCESS);

  PACL newDacl = nullptr;
  DWORD err = SetEntriesInAclA(n, eas, nullptr, &newDacl);
  if (err != ERROR_SUCCESS || !newDacl) {
    errOut = "secureSecretsDir: SetEntriesInAclA failed";
    if (newDacl) LocalFree(newDacl);
    LocalFree(realSid);
    return false;
  }
  err = SetNamedSecurityInfoA(const_cast<char*>(dir.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION |
                                  PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, newDacl, nullptr);
  LocalFree(newDacl);
  LocalFree(realSid);
  if (err != ERROR_SUCCESS) {
    errOut = "secureSecretsDir: SetNamedSecurityInfoA failed";
    return false;
  }
  return true;
}

bool hideUserFromLogin() {
  // HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon\
  //   SpecialAccounts\UserList : <name> = 0
  HKEY key = nullptr;
  LONG rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
                          "Winlogon\\SpecialAccounts\\UserList",
                          0, KEY_SET_VALUE, &key);
  if (rc != ERROR_SUCCESS) {
    // Create the key path.
    HKEY parent = nullptr;
    rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
                       "Winlogon\\SpecialAccounts",
                       0, KEY_SET_VALUE, &parent);
    if (rc != ERROR_SUCCESS) return false;
    rc = RegCreateKeyExA(parent, "UserList", 0, nullptr, 0, KEY_SET_VALUE,
                         nullptr, &key, nullptr);
    RegCloseKey(parent);
    if (rc != ERROR_SUCCESS) return false;
  }
  DWORD zero = 0;
  rc = RegSetValueExA(key, "AoiSandboxUser", 0, REG_DWORD,
                      reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

} // namespace

namespace {
void logSetupError(const std::string& workdir, const std::string& msg) {
  std::ofstream f(workdir + "\\sandbox_setup_err.txt", std::ios::app);
  if (f) f << msg << "\n";
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: aoi-sandbox-setup <workdir> <realUserSid>\n");
    return 1;
  }
  const std::string workdir = argv[1];
  const std::string realUserSid = argv[2];
  logSetupError(workdir, "=== setup start ===");

  // Must be elevated.
  {
    BOOL elevated = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      TOKEN_ELEVATION te{};
      DWORD n = 0;
      if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n))
        elevated = te.TokenIsElevated;
      CloseHandle(tok);
    }
    if (!elevated) {
      fprintf(stderr, "aoi-sandbox-setup must run elevated\n");
      return 2;
    }
  }

  const std::wstring wWorkdir = utf8ToWide(workdir);

  // Reuse the existing password when it still works: several packages share
  // the single sandbox user, and rotating on every rerun would invalidate
  // the other packages' secrets ("一直提权" loop). Only rotate when absent
  // or stale.
  std::string password = existingPassword(workdir);
  const bool setPassword = !passwordValid(password);
  if (setPassword) password = randomPassword();

  if (!provisionUsers(password, setPassword)) {
    logSetupError(workdir, "provisionUsers failed");
    return 3;
  }
  logSetupError(workdir, setPassword ? "provisionUsers ok (password rotated)"
                                     : "provisionUsers ok (password reused)");
  if (!grantSandboxPrivileges()) {
    logSetupError(workdir, "grantSandboxPrivileges failed");
    return 10;
  }
  logSetupError(workdir, "grantSandboxPrivileges ok");
  hideUserFromLogin();
  logSetupError(workdir, "hideUserFromLogin ok");

  // Store the DPAPI-encrypted password. ACLs and the user SID are handled on
  // the agent side (it owns the directories and can secure them without
  // elevation), which also avoids the sechost.dll crash seen with
  // LookupAccountName in elevated processes.
  {
    const std::string secDir = workdir + "\\" + kSecretsDir;
    CreateDirectoryA(secDir.c_str(), nullptr);
    const std::string file = secDir + "\\" + kSecretsFile;
    const std::string b64 = dpapiEncrypt(password);
    if (b64.empty()) {
      logSetupError(workdir, "CryptProtectData failed");
      return 6;
    }
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
      logSetupError(workdir, "cannot write secrets file: " + file);
      return 7;
    }
    f << "{\"version\":1,\"user\":\"AoiSandboxUser\",\"password_b64\":\"" << b64
      << "\"}\n";
    f.close();
    std::string aclErr;
    if (!secureSecretsDir(workdir, realUserSid, aclErr)) {
      logSetupError(workdir, aclErr);
      return 8;
    }
  }
  logSetupError(workdir, "secrets written + secured");

  // Initialize the sandbox user's profile COMPLETELY. CreateProcessWithLogonW's
  // LOGON_WITH_PROFILE on a freshly-created user leaves an EMPTY profile dir
  // (no NTUSER.DAT) and HKU mounts a temporary hive; schannel then fails in
  // WRITE_RESTRICTED children with SEC_E_NO_CREDENTIALS (no per-user cert
  // store to open). LoadUserProfile here - with SeRestore/SeBackup ENABLED on
  // the elevated token (LoadUserProfile requires them; "merely impersonating
  // an administrator is not sufficient" - MS Learn) - creates the real
  // NTUSER.DAT + directory skeleton; later logons mount it and TLS works.
  {
    // 1) Enable the required privileges on OUR token (present but disabled).
    HANDLE self = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &self)) {
      const auto enable = [self](const char* name) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueA(nullptr, name, &tp.Privileges[0].Luid))
          AdjustTokenPrivileges(self, FALSE, &tp, 0, nullptr, nullptr);
      };
      enable("SeRestorePrivilege");
      enable("SeBackupPrivilege");
      CloseHandle(self);
    }

    // 2) Only rebuild when the profile is missing/broken (no NTUSER.DAT).
    const std::wstring profileDir = L"C:\\Users\\" + std::wstring(kUserName);
    std::error_code pec;
    const bool needProfile =
        !std::filesystem::exists(profileDir + L"\\NTUSER.DAT", pec);
    if (needProfile) {
      // Best-effort removal of the empty/partial dir so LoadUserProfile
      // creates a complete one (fails silently if a session still holds it).
      std::error_code rec;
      std::filesystem::remove_all(profileDir, rec);
      HANDLE logon = nullptr;
      if (LogonUserW(kUserName, L".", utf8ToWide(password).c_str(),
                     LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT,
                     &logon)) {
        PROFILEINFOW pi{};
        pi.dwSize = sizeof(pi);
        wchar_t uname[] = L"AoiSandboxUser";
        pi.lpUserName = uname;
        if (LoadUserProfileW(logon, &pi)) {
          logSetupError(workdir, "LoadUserProfile ok (profile created)");
          UnloadUserProfile(logon, pi.hProfile);
        } else {
          logSetupError(workdir, "LoadUserProfile failed: err=" +
                                     std::to_string(GetLastError()));
        }
        CloseHandle(logon);
      } else {
        logSetupError(workdir, "LogonUser for profile failed");
      }
    } else {
      logSetupError(workdir, "profile already complete (NTUSER.DAT present)");
    }
  }

  printf("sandbox setup complete\n");
  return 0;
}
