#include "sqlite_tools.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <string>
#include <vector>

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

// Run a read-only query; on success returns the rows (each a JSON object of
// column name -> value) and leaves `err` empty.
nlohmann::json queryRows(const std::string& path, const std::string& sql,
                         std::string& err) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    err = db ? sqlite3_errmsg(db) : "cannot open database";
    if (db) sqlite3_close(db);
    return nlohmann::json::array();
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    sqlite3_close(db);
    return nlohmann::json::array();
  }

  const int nCols = sqlite3_column_count(stmt);
  std::vector<std::string> colNames;
  colNames.reserve(static_cast<size_t>(nCols));
  for (int i = 0; i < nCols; ++i) {
    const char* name = sqlite3_column_name(stmt, i);
    colNames.emplace_back(name ? name : "");
  }

  nlohmann::json rows = nlohmann::json::array();
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    nlohmann::json row = nlohmann::json::object();
    for (int i = 0; i < nCols; ++i) {
      const std::string& name = colNames[static_cast<size_t>(i)];
      switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER:
          row[name] = sqlite3_column_int64(stmt, i);
          break;
        case SQLITE_FLOAT:
          row[name] = sqlite3_column_double(stmt, i);
          break;
        case SQLITE_NULL:
          row[name] = nullptr;
          break;
        default: {
          const unsigned char* text = sqlite3_column_text(stmt, i);
          row[name] = text ? reinterpret_cast<const char*>(text) : "";
          break;
        }
      }
    }
    rows.push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) err = sqlite3_errmsg(db);

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return rows;
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

      std::string err;
      nlohmann::json rows = queryRows(path, sql, err);
      if (!err.empty()) {
        return nlohmann::json{{"content", "(sql_query failed: " + err + ")"}};
      }

      nlohmann::json out = {{"db", path}, {"count", rows.size()}, {"rows", rows}};
      std::string text = out.dump();
      constexpr size_t kMax = 30000;
      if (text.size() > kMax) {
        text.resize(kMax);
        text += "\n...(truncated)";
      }
      return nlohmann::json{{"content", text}};
    } catch (const std::exception& ex) {
      return nlohmann::json{{"content", std::string("(sql_query failed: ") + ex.what() + ")"}};
    }
  };
  return t;
}

} // namespace aoi
