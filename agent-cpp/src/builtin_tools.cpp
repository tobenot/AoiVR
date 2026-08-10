#include "builtin_tools.hpp"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>


namespace aoi {

namespace {

// Read a file as text, truncated to a max length with a marker.
std::string readTextFile(const std::string& path, size_t maxBytes = 60000) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return "(file not found: " + path + ")";
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string out = ss.str();
  if (out.size() > maxBytes) {
    out.resize(maxBytes);
    out += "\n...(truncated)";
  }
  return out;
}

std::string runCommand(const std::string& cmd, long timeoutMs = 30000) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE hRead, hWrite;
  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "(CreatePipe failed)";
  SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;
  si.dwFlags = STARTF_USESTDHANDLES;
  PROCESS_INFORMATION pi{};
  std::string fullCmd = "cmd.exe /c " + cmd;
  std::vector<char> cmdBuf(fullCmd.begin(), fullCmd.end());
  cmdBuf.push_back('\0');
  if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(hRead);
    CloseHandle(hWrite);
    return "(CreateProcess failed)";
  }
  CloseHandle(hWrite);

  std::string out;
  char buf[4096];
  DWORD n = 0;
  // Read the child's output on a helper thread (bounded by the timeout), then
  // join it before returning. The reader writes into a HEAP buffer (shared
  // ptr) so that even if the wait times out and the handle is closed, the
  // reader never touches a dead stack object.
  struct ReaderArgs {
    HANDLE h;
    std::shared_ptr<std::string> out;
    std::atomic<bool>* done;
  };
  auto outBuf = std::make_shared<std::string>();
  std::atomic<bool> readerDone{false};
  ReaderArgs args{hRead, outBuf, &readerDone};
  DWORD tid = 0;
  HANDLE readerThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
    auto* a = static_cast<ReaderArgs*>(p);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(a->h, buf, sizeof(buf), &n, nullptr) && n > 0) {
      a->out->append(buf, n);
    }
    a->done->store(true);
    return 0;
  }, &args, 0, &tid);

  if (!readerThread) {
    // Could not spawn reader; read synchronously (child output is small here).
    while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
      out.append(buf, n);
    }
  } else {
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
      TerminateProcess(pi.hProcess, 1);
    }
    // The child handle is now signaled (or terminated); the pipe will EOF and
    // the reader thread exits on its own. Wait for it before returning so the
    // shared buffer is fully populated.
    WaitForSingleObject(readerThread, 5000);
    CloseHandle(readerThread);
    out = *outBuf;
  }
  CloseHandle(hRead);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (out.empty()) out = "(no output)";
  if (out.size() > 30000) { out.resize(30000); out += "\n...(truncated)"; }
  return out;
}

} // namespace

ToolDefinition makeReadTool() {
  ToolDefinition t;
  t.name = "read";
  t.label = "read";
  t.description = "Read a file from disk and return its contents. Use for inspecting files, configs, or source code. Always provide the required 'path' parameter.";
  t.parameters = {
      {"type", "object"},
      {"properties", nlohmann::json{{"path", {{"type", "string"}, {"description", "Absolute or relative file path"}}}}},
      {"required", nlohmann::json::array({"path"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    const std::string path = args.value("path", "");
    if (path.empty())
      return nlohmann::json{{"content", "(read: missing required parameter 'path' - call the tool again with the file path you want to read)"}};
    return nlohmann::json{{"content", readTextFile(path)}};
  };
  return t;
}

ToolDefinition makeBashTool() {
  ToolDefinition t;
  t.name = "bash";
  t.label = "bash";
  t.description =
      "Run a shell command on the user's Windows machine and return its output. Use sparingly and only for what the user asked. "
      "Tips: get the current time with `date /t` (always available); for network requests use `curl -s --max-time 15 <url>` "
      "and prefer https endpoints that are reachable from China (e.g. baidu.com). If an https request fails with exit 35, "
      "the endpoint's TLS is unreachable - try another host or use `curl -k`.";
  t.parameters = {
      {"type", "object"},
      {"properties", nlohmann::json{{"command", {{"type", "string"}, {"description", "The command to run"}}}}},
      {"required", nlohmann::json::array({"command"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    const std::string cmd = args.value("command", "");
    return nlohmann::json{{"content", runCommand(cmd)}};
  };
  return t;
}

ToolDefinition makeEditTool() {
  ToolDefinition t;
  t.name = "edit";
  t.label = "edit";
  t.description = "Replace an exact occurrence of old_string with new_string in a file. Returns success or an error.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{{"path", {{"type", "string"}}},
                      {"old_string", {{"type", "string"}}},
                      {"new_string", {{"type", "string"}}}}},
      {"required", nlohmann::json::array({"path", "old_string", "new_string"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    const std::string path = args.value("path", "");
    const std::string oldS = args.value("old_string", "");
    const std::string newS = args.value("new_string", "");
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return nlohmann::json{{"content", "(file not found: " + path + ")"}};
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();
    std::string text = ss.str();
    const size_t pos = text.find(oldS);
    if (pos == std::string::npos)
      return nlohmann::json{{"content", "(old_string not found in file)"}};
    text.replace(pos, oldS.size(), newS);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return nlohmann::json{{"content", "(cannot write file: " + path + ")"}};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return nlohmann::json{{"content", "Edit applied."}};
  };
  return t;
}

ToolDefinition makeWriteTool() {
  ToolDefinition t;
  t.name = "write";
  t.label = "write";
  t.description = "Write content to a file (creates or overwrites).";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{{"path", {{"type", "string"}}},
                      {"content", {{"type", "string"}}}}},
      {"required", nlohmann::json::array({"path", "content"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    const std::string path = args.value("path", "");
    const std::string content = args.value("content", "");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return nlohmann::json{{"content", "(cannot write file: " + path + ")"}};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return nlohmann::json{{"content", "File written."}};
  };
  return t;
}

} // namespace aoi
