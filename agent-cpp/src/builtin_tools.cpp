#include "builtin_tools.hpp"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>

#include "windows_sandbox.hpp"

namespace aoi {

namespace {

// Run a tool operation inside the sandbox (aligned with Codex exec-server:
// the agent process never executes model-controlled writes itself; everything
// goes through the sandboxed helper, and the OS enforces the workspace-only
// writable policy). Returns the tool result in the standard {"content": ...}
// shape; sandbox failures and helper errors are surfaced to the model.
nlohmann::json sandboxToolResult(const std::string& opJson) {
  const std::string reply = sandboxExecute(opJson);
  auto j = nlohmann::json::parse(reply, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("ok")) {
    return nlohmann::json{{"content", reply}};
  }
  if (j["ok"].is_boolean() && j["ok"].get<bool>()) {
    return nlohmann::json{{"content", j.value("output", "")}};
  }
  return nlohmann::json{{"content", j.value("error", "(sandbox error)")}};
}

} // namespace

ToolDefinition makeReadTool() {
  ToolDefinition t;
  t.name = "read";
  t.label = "read";
  t.description =
      "Read a file from disk and return its contents. Use for inspecting files, configs, or source code. "
      "Large files are paged: pass 'offset' (bytes) to continue where a previous read stopped - the result "
      "tells you the next offset. Tool outputs from bash/sql_query/fetch that exceed their size cap are "
      "saved in full to sandbox\\out\\ - read those files with offset to see everything.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"path", {{"type", "string"}, {"description", "Absolute or relative file path"}}},
           {"offset", {{"type", "integer"},
                       {"description", "Byte offset to start reading from (default 0)"}}},
           {"max_bytes", {{"type", "integer"},
                          {"description", "Max bytes to return per call (default 60000)"}}}}},
      {"required", nlohmann::json::array({"path"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    const std::string path = args.value("path", "");
    if (path.empty())
      return nlohmann::json{{"content", "(read: missing required parameter 'path' - call the tool again with the file path you want to read)"}};
    auto op = nlohmann::json{{"op", "read"}, {"path", path}};
    // value(key, int) throws type_error when the model sends a string/float;
    // copy the raw JSON instead so the helper validates it.
    if (args.contains("offset")) op["offset"] = args["offset"];
    if (args.contains("max_bytes")) op["max_bytes"] = args["max_bytes"];
    return sandboxToolResult(op.dump());
  };
  return t;
}

ToolDefinition makeBashTool() {
  ToolDefinition t;
  t.name = "bash";
  t.label = "bash";
  t.description =
      "Run a shell command on the user's Windows machine and return its output. Use sparingly and only for what the user asked. "
      "Tips: get the current time with `date /t` (always available). "
      "NEVER use curl/wget/Invoke-WebRequest/Invoke-RestMethod for network requests: the sandbox cannot do TLS "
      "(curl exits with error 35), so those always fail. Use the `fetch` tool instead - it supports custom headers, "
      "cookies, GET/POST and works reliably. "
      "Command output over 30KB is saved in full to the sandbox workspace out\\ directory (path is returned); "
      "page through it with the read tool's offset parameter.";
  t.parameters = {
      {"type", "object"},
      {"properties", nlohmann::json{{"command", {{"type", "string"}, {"description", "The command to run"}}}}},
      {"required", nlohmann::json::array({"command"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    const std::string cmd = args.value("command", "");
    return sandboxToolResult(nlohmann::json{{"op", "bash"}, {"command", cmd}}.dump());
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
    return sandboxToolResult(
        nlohmann::json{{"op", "edit"},
                       {"path", args.value("path", "")},
                       {"old_string", args.value("old_string", "")},
                       {"new_string", args.value("new_string", "")}}
            .dump());
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
    return sandboxToolResult(
        nlohmann::json{{"op", "write"},
                       {"path", args.value("path", "")},
                       {"content", args.value("content", "")}}
            .dump());
  };
  return t;
}

} // namespace aoi
