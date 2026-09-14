#include "sqlite_tools.hpp"

#include <cstdlib>
#include <cstring>
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

// Expand %VAR% references using the AGENT process environment. The sandbox
// user's environment differs (%APPDATA% points at ITS profile), so a literal
// "%APPDATA%\..." passed by the model must be resolved HERE, before the path
// reaches the sandboxed helper.
std::string expandEnv(const std::string& in) {
  std::string out = in;
  size_t pos = 0;
  while ((pos = out.find('%', pos)) != std::string::npos) {
    const size_t end = out.find('%', pos + 1);
    if (end == std::string::npos) break;
    const std::string name = out.substr(pos + 1, end - pos - 1);
    if (name.empty()) {
      pos = end + 1;
      continue;
    }
    const char* v = std::getenv(name.c_str());
    if (v && *v) {
      out.replace(pos, end - pos + 1, v);
      pos += std::strlen(v);
    } else {
      pos = end + 1;
    }
  }
  return out;
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
      "Run a read-only SELECT or PRAGMA query against the VRCX companion app's VRChat "
      "session database and return the result rows as JSON. The database is opened strictly "
      "READ-ONLY (writes are impossible) and the path is FIXED to the configured VRCX store "
      "(%APPDATA%\\VRCX\\VRCX.sqlite3, or aoi_config.json vrcxDbPath) - no other database can "
      "be queried. Its `cookies` table contains the logged-in VRChat auth session as a "
      "base64-encoded JSON list of cookies (see the convert tool to decode it). "
      "Other tables can be discovered with `SELECT name FROM sqlite_master WHERE type='table'`. "
      "Results over 30KB are saved in full to the sandbox workspace out\\ directory (path is "
      "returned); page through them with the read tool's offset parameter.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"sql",
            {{"type", "string"},
             {"description",
              "Read-only SQL statement (must start with SELECT, WITH or PRAGMA)."}}}}},
      {"required", nlohmann::json::array({"sql"})},
  };
  t.execute = [configuredDbPath](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    try {
      // Path is FIXED to the configured VRCX database - the model cannot
      // choose an arbitrary file to read (sandbox user is world-readable,
      // so an unrestricted db_path would let it query any .sqlite on disk).
      std::string path = configuredDbPath;
      if (path.empty()) {
        path = defaultVrcxPath();
        if (path.empty()) {
          return nlohmann::json{{"content", "(sql_query failed: %APPDATA% not set)"}};
        }
      }
      // Resolve %VAR% (e.g. "%APPDATA%\VRCX\VRCX.sqlite3") in the AGENT
      // environment - the sandbox user's %APPDATA% points at ITS profile.
      path = expandEnv(path);

      const std::string sql = trimLeft(args.value("sql", ""));
      if (sql.empty()) {
        return nlohmann::json{{"content", "(sql_query rejected: empty SQL)"}};
      }
      // Strict read-only prefix check: the statement must start with
      // SELECT / WITH / PRAGMA followed by whitespace or a quote (so
      // "SELECTED ..." or "PRAGMAATTACH ..." cannot slip through).
      // Strict read-only prefix check: the statement must start with
      // SELECT / WITH / PRAGMA followed by whitespace/quote/paren (so
      // "SELECTED ..." or "PRAGMAATTACH ..." cannot slip through).
      auto kwEndOk = [&sql](const char* kw, size_t len) {
        if (sql.rfind(kw, 0) != 0) return false;
        if (sql.size() == len) return true;
        const unsigned char c = static_cast<unsigned char>(sql[len]);
        return isspace(c) != 0 || c == '"' || c == '\'' || c == '(';
      };
      const bool select =
          kwEndOk("SELECT", 6) || kwEndOk("select", 6) || kwEndOk("WITH", 4) ||
          kwEndOk("with", 4);
      const bool pragma = kwEndOk("PRAGMA", 6) || kwEndOk("pragma", 6);
      if (!select && !pragma) {
        return nlohmann::json{
            {"content", "(sql_query rejected: only SELECT/WITH/PRAGMA statements are allowed)"}};
      }
      // Reject ATTACH outright (could open another database in a read-only
      // session) and any write verbs that follow a leading comment.
      const std::string up = sql;
      if (up.find("ATTACH") != std::string::npos ||
          up.find("attach") != std::string::npos) {
        return nlohmann::json{{"content", "(sql_query rejected: ATTACH is not allowed)"}};
      }

      // Execute inside the sandbox: the agent process never runs model-
      // controlled operations itself; the helper enforces read-only SQLite.
      const std::string reply =
          sandboxExecute(nlohmann::json{{"op", "sql_query"},
                                        {"sql", sql},
                                        {"db_path", path},
                                        {"allowed_db", path}}
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
