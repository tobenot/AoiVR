#include "sqlite_tools.hpp"

#include <cstdlib>
#include <string>

#include "windows_sandbox.hpp"

namespace aoi {

namespace {

// Default location of the VRCX companion app database (Windows). VRCX keeps
// the user's VRChat auth session here, see the `cookies` table.
std::string defaultVrcxPath() {
  const char* appdata = std::getenv("APPDATA");
  if (!appdata || !*appdata) return "";
  return std::string(appdata) + "\\VRCX\\VRCX.sqlite3";
}

std::string trimLeft(const std::string& s) {
  const size_t pos = s.find_first_not_of(" \t\r\n");
  return pos == std::string::npos ? std::string() : s.substr(pos);
}

} // namespace

ToolDefinition makeSqlQueryTool(const std::string& configuredDbPath) {
  ToolDefinition t;
  t.name = "sql_query";
  t.label = "sql_query";
  t.description =
      "Run a read-only SELECT or PRAGMA query against a local SQLite database and return the "
      "result rows as JSON. The database is opened strictly READ-ONLY (writes are impossible). "
      "Default database (omit db_path): the VRCX companion app's VRChat session store at "
      "%APPDATA%\\VRCX\\VRCX.sqlite3 - its `cookies` table contains the logged-in VRChat auth "
      "session as a base64-encoded JSON list of cookies (see the convert tool to decode it). "
      "Other tables can be discovered with `SELECT name FROM sqlite_master WHERE type='table'`.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"db_path",
            {{"type", "string"},
             {"description",
              "Path to the .sqlite3 file. Omit to use the default VRCX database "
              "(%APPDATA%\\VRCX\\VRCX.sqlite3)."}}},
           {"sql",
            {{"type", "string"},
             {"description", "Read-only SQL statement (must start with SELECT or PRAGMA)."}}}}},
      {"required", nlohmann::json::array({"sql"})},
  };
  t.execute = [configuredDbPath](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    try {
      std::string path = args.value("db_path", "");
      if (path.empty()) path = configuredDbPath;
      if (path.empty()) {
        path = defaultVrcxPath();
        if (path.empty()) {
          return nlohmann::json{{"content", "(sql_query failed: %APPDATA% not set)"}};
        }
      }

      const std::string sql = trimLeft(args.value("sql", ""));
      if (sql.empty()) {
        return nlohmann::json{{"content", "(sql_query rejected: empty SQL)"}};
      }
      const bool select = sql.rfind("SELECT", 0) == 0 || sql.rfind("select", 0) == 0;
      const bool pragma = sql.rfind("PRAGMA", 0) == 0 || sql.rfind("pragma", 0) == 0;
      if (!select && !pragma) {
        return nlohmann::json{
            {"content", "(sql_query rejected: only SELECT/PRAGMA statements are allowed)"}};
      }

      // Execute inside the sandbox: the agent process never runs model-
      // controlled operations itself; the helper enforces read-only SQLite.
      const std::string reply =
          sandboxExecute(nlohmann::json{{"op", "sql_query"},
                                        {"sql", sql},
                                        {"db_path", path}}
                             .dump());
      auto j = nlohmann::json::parse(reply, nullptr, false);
      if (!j.is_discarded() && j.is_object() && j.contains("ok") &&
          j["ok"].is_boolean() && j["ok"].get<bool>()) {
        return nlohmann::json{{"content", j.value("output", "")}};
      }
      return nlohmann::json{
          {"content", !j.is_discarded() && j.is_object()
                          ? j.value("error", "(sandbox error)")
                          : reply}};
    } catch (const std::exception& ex) {
      return nlohmann::json{{"content", std::string("(sql_query failed: ") + ex.what() + ")"}};
    }
  };
  return t;
}

} // namespace aoi
