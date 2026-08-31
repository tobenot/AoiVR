#include "hook_scheduler.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <vector>

#include "windows_sandbox.hpp"

namespace aoi {

namespace {

std::string makeId() {
  std::mt19937_64 rng(static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count()) ^
      reinterpret_cast<uintptr_t>(&rng));
  const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(32);
  for (int i = 0; i < 32; ++i) s += hex[(rng() >> ((i % 8) * 4)) & 0xF];
  return s;
}

bool parseDateTime(const std::string& iso, struct tm* out) {
  // Expect "YYYY-MM-DDTHH:MM:SSZ"
  if (iso.size() < 19) return false;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &out->tm_year, &out->tm_mon,
                  &out->tm_mday, &out->tm_hour, &out->tm_min, &out->tm_sec) != 6)
    return false;
  out->tm_year -= 1900;
  out->tm_mon -= 1;
  out->tm_isdst = -1;
  // Normalize so derived fields (tm_yday/tm_wday) are populated; without this
  // tm_yday stays 0 and day-level comparisons silently rely on tm_year alone.
  std::mktime(out);
  return true;
}

} // namespace

HookScheduler::HookScheduler(const std::string& dbPath, HookConfig cfg)
    : dbPath_(dbPath), cfg_(cfg) {}

HookScheduler::~HookScheduler() { stop(); }

void HookScheduler::setCallbacks(LlmCallback llm, NotifyCallback notify,
                                 LogCallback log) {
  std::lock_guard<std::mutex> lk(mtx_);
  llmCb_ = std::move(llm);
  notifyCb_ = std::move(notify);
  logCb_ = std::move(log);
}

std::string HookScheduler::nowIso() const {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &tt);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  return buf;
}

bool HookScheduler::openDb() {
  if (db_) return true;
  if (sqlite3_open_v2(dbPath_.c_str(), &db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    if (db_) {
      if (logCb_) logCb_("(hooks: cannot open db: " +
                         std::string(sqlite3_errmsg(db_)) + ")");
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }
  ensureSchema();
  return true;
}

void HookScheduler::ensureSchema() {
  if (!db_) return;
  const char* ddl =
      "CREATE TABLE IF NOT EXISTS agent_hooks ("
      " id TEXT PRIMARY KEY,"
      " name TEXT NOT NULL,"
      " trigger_type TEXT NOT NULL DEFAULT 'interval',"
      " interval_seconds INTEGER,"
      " at_hour INTEGER, at_minute INTEGER,"
      " action_type TEXT NOT NULL,"
      " intent TEXT,"
      " script_path TEXT,"
      " context TEXT DEFAULT '{}',"
      " enabled INTEGER DEFAULT 1,"
      " status TEXT DEFAULT 'active',"
      " last_fired_at TEXT, last_error TEXT,"
      " created_by TEXT, created_at TEXT"
      ");";
  char* err = nullptr;
  if (sqlite3_exec(db_, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
    if (logCb_) logCb_("(hooks: schema error: " + std::string(err ? err : "?") +
                       ")");
    if (err) sqlite3_free(err);
  }
}

std::vector<HookScheduler::HookRow> HookScheduler::loadHooks() {
  std::vector<HookRow> out;
  if (!db_) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT id,name,trigger_type,interval_seconds,at_hour,"
                         "at_minute,action_type,intent,script_path,context,"
                         "enabled,status,last_fired_at,last_error FROM "
                         "agent_hooks",
                         -1, &stmt, nullptr) != SQLITE_OK)
    return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    HookRow h;
    const auto col = [&](int i) -> std::string {
      const unsigned char* t = sqlite3_column_text(stmt, i);
      return t ? reinterpret_cast<const char*>(t) : "";
    };
    h.id = col(0);
    h.name = col(1);
    h.triggerType = col(2);
    h.intervalSeconds = sqlite3_column_int64(stmt, 3);
    h.atHour = sqlite3_column_int(stmt, 4);
    h.atMinute = sqlite3_column_int(stmt, 5);
    h.actionType = col(6);
    h.intent = col(7);
    h.scriptPath = col(8);
    h.context = col(9);
    h.enabled = sqlite3_column_int(stmt, 10) != 0;
    h.status = col(11);
    h.lastFiredAt = col(12);
    h.lastError = col(13);
    out.push_back(std::move(h));
  }
  sqlite3_finalize(stmt);
  return out;
}

bool HookScheduler::saveHook(const HookRow& h) {
  if (!db_) return false;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT OR REPLACE INTO agent_hooks (id,name,trigger_type,"
      "interval_seconds,at_hour,at_minute,action_type,intent,script_path,"
      "context,enabled,status,last_fired_at,last_error,created_by,created_at)"
      " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  const auto bind = [&](int i, const std::string& v) {
    sqlite3_bind_text(stmt, i, v.c_str(), -1, SQLITE_TRANSIENT);
  };
  bind(1, h.id);
  bind(2, h.name);
  bind(3, h.triggerType);
  sqlite3_bind_int64(stmt, 4, h.intervalSeconds);
  sqlite3_bind_int(stmt, 5, h.atHour);
  sqlite3_bind_int(stmt, 6, h.atMinute);
  bind(7, h.actionType);
  bind(8, h.intent);
  bind(9, h.scriptPath);
  bind(10, h.context);
  sqlite3_bind_int(stmt, 11, h.enabled ? 1 : 0);
  bind(12, h.status);
  bind(13, h.lastFiredAt);
  bind(14, h.lastError);
  bind(15, "hook_manage");
  bind(16, nowIso());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool HookScheduler::updateFired(const std::string& id,
                                const std::string& firedAt,
                                const std::string& error,
                                const std::string& status) {
  if (!db_) return false;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE agent_hooks SET last_fired_at=?, last_error=?, status=? WHERE "
      "id=?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, firedAt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, error.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

// ---- hook_manage tool ----
nlohmann::json HookScheduler::manage(const nlohmann::json& args) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (!openDb()) {
    return {{"content", "(hook_manage failed: cannot open hook database)"}};
  }
  const std::string action = args.value("action", "");
  const std::string name = args.value("name", "");

  if (action == "create") {
    // Guardrail: hook count limit.
    const auto hooks = loadHooks();
    if (static_cast<int>(hooks.size()) >= cfg_.maxHooks) {
      return {{"content", "(hook_manage rejected: hook count limit reached (" +
                              std::to_string(cfg_.maxHooks) + "))"}};
    }
    const std::string triggerType = args.value("trigger", nlohmann::json::object())
                                        .value("type", "interval");
    long long intervalSeconds = 0;
    int atHour = -1, atMinute = -1;
    if (triggerType == "interval") {
      const nlohmann::json t = args.value("trigger", nlohmann::json::object());
      intervalSeconds = t.is_object() ? t.value("interval_seconds", 0LL) : 0LL;
      if (intervalSeconds < 60) {
        return {{"content", "(hook_manage rejected: interval_seconds must be "
                             ">= 60)"}};
      }
    } else if (triggerType == "at") {
      const nlohmann::json t = args.value("trigger", nlohmann::json::object());
      const std::string at = t.is_object() ? t.value("at", "") : "";
      if (std::sscanf(at.c_str(), "%d:%d", &atHour, &atMinute) != 2 ||
          atHour < 0 || atHour > 23 || atMinute < 0 || atMinute > 59) {
        return {{"content", "(hook_manage rejected: invalid at time, use "
                             "HH:MM)"}};
      }
    } else {
      return {{"content", "(hook_manage rejected: unknown trigger type '" +
                              triggerType + "')"}};
    }
    const std::string actionType = args.value("action_type", "llm");
    if (actionType != "llm" && actionType != "script" &&
        actionType != "llm_script") {
      return {{"content", "(hook_manage rejected: unknown action_type '" +
                              actionType + "')"}};
    }
    std::string scriptPath = args.value("script_path", "");
    if ((actionType == "script" || actionType == "llm_script") &&
        !scriptPath.empty()) {
      // White-list: sandbox-space relative path only; no absolute paths, no
      // "..", no drive letters, no leading separators, and NO cmd metacharacters
      // (" & | < > ^ etc.) - the path is embedded in a cmd command line, so any
      // of them would inject additional commands (sandbox user, but still).
      const auto isPathChar = [](char c) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        return alnum || c == '_' || c == '-' || c == '.' || c == '\\' ||
               c == '/' || c == ' ' || c == '(' || c == ')' || c == '@' ||
               c == '$';
      };
      bool okChars = true;
      for (const char c : scriptPath)
        if (!isPathChar(c)) { okChars = false; break; }
      if (scriptPath.find("..") != std::string::npos ||
          scriptPath.find(':') != std::string::npos ||
          (!scriptPath.empty() && (scriptPath[0] == '/' || scriptPath[0] == '\\')) ||
          !okChars) {
        return {{"content", "(hook_manage rejected: script_path must be a "
                             "sandbox-space relative path; no '..', absolute "
                             "paths, drive letters or shell metacharacters)"}};
      }
    }
    HookRow h;
    h.id = makeId();
    h.name = name.empty() ? h.id : name;
    h.triggerType = triggerType;
    h.intervalSeconds = intervalSeconds;
    h.atHour = atHour;
    h.atMinute = atMinute;
    h.actionType = actionType;
    h.intent = args.value("intent", "");
    h.scriptPath = scriptPath;
    h.context = args.value("context", nlohmann::json::object()).dump();
    h.enabled = true;
    h.status = "active";
    if (!saveHook(h)) {
      return {{"content", "(hook_manage failed: cannot persist hook)"}};
    }
    if (logCb_)
      logCb_("(hooks) created hook id=" + h.id + " name=" + h.name +
             " trigger=" + triggerType + " action=" + actionType);
    return {{"content", "Hook created. id=" + h.id + " name=" + h.name}};
  }

  if (action == "list") {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& h : loadHooks()) {
      nlohmann::json j;
      j["id"] = h.id;
      j["name"] = h.name;
      j["trigger_type"] = h.triggerType;
      j["interval_seconds"] = h.intervalSeconds;
      j["at"] = h.atHour >= 0
                    ? (std::to_string(h.atHour) + ":" +
                       (h.atMinute < 10 ? "0" : "") + std::to_string(h.atMinute))
                    : "";
      j["action_type"] = h.actionType;
      j["intent"] = h.intent;
      j["script_path"] = h.scriptPath;
      j["enabled"] = h.enabled;
      j["status"] = h.status;
      j["last_fired_at"] = h.lastFiredAt;
      j["last_error"] = h.lastError;
      arr.push_back(std::move(j));
    }
    return {{"content", arr.dump(2)}};
  }

  // cancel / pause / resume: find by id or name.
  std::vector<HookRow> hooks = loadHooks();
  HookRow* target = nullptr;
  for (auto& h : hooks) {
    if (h.id == name || h.name == name) {
      target = &h;
      break;
    }
  }
  if (!target) {
    return {{"content", "(hook_manage: no hook found with id/name '" + name +
                            "')"}};
  }
  if (action == "cancel") {
    if (db_) {
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db_, "DELETE FROM agent_hooks WHERE id=?",
                             -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, target->id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
    if (logCb_) logCb_("(hooks) cancelled hook id=" + target->id);
    return {{"content", "Hook cancelled."}};
  }
  const std::string newStatus = action == "pause" ? "paused" : "active";
  if (action == "pause" || action == "resume") {
    // Status-only UPDATE: reusing updateFired would also clear last_error and
    // rewrite last_fired_at with the old value round-tripped through string
    // parsing (and pause diagnostics would be lost).
    if (db_) {
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db_, "UPDATE agent_hooks SET status=? WHERE id=?",
                             -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, newStatus.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, target->id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
    if (logCb_)
      logCb_("(hooks) " + action + " hook id=" + target->id);
    return {{"content", "Hook " + action + "d."}};
  }
  return {{"content", "(hook_manage: unknown action '" + action + "')"}};
}

// ---- scheduler ----
void HookScheduler::start() {
  std::lock_guard<std::mutex> lk(mtx_);
  if (running_) return;
  running_ = true;
  thread_ = std::thread([this] { schedulerLoop(); });
}

void HookScheduler::stop() {
  std::thread t;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!running_) return;
    running_ = false;
    t = std::move(thread_);
  }
  cv_.notify_all();
  if (t.joinable()) t.join();
  std::lock_guard<std::mutex> lk(mtx_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool HookScheduler::isSilentHour(int hour) const {
  if (cfg_.silentStart <= cfg_.silentEnd)
    return hour >= cfg_.silentStart && hour < cfg_.silentEnd;
  return hour >= cfg_.silentStart || hour < cfg_.silentEnd;  // wraps midnight
}

void HookScheduler::schedulerLoop() {
  std::unique_lock<std::mutex> lk(mtx_);
  while (running_) {
    // Wake at least every second to check second-granular triggers.
    cv_.wait_for(lk, std::chrono::seconds(1), [this] { return !running_; });
    if (!running_) break;
    // Collect due hooks while holding the lock.
    std::vector<DispatchJob> jobs;
    collectDueHooks(jobs);
    // Dispatch OUTSIDE the lock: the callbacks run a full LLM sub-session with
    // the same tool set (including hook_manage -> manage() takes mtx_). Firing
    // them while holding the lock would deadlock the scheduler thread forever.
    if (!jobs.empty()) {
      lk.unlock();
      for (const auto& j : jobs) {
        // Any exception escaping runScript/runLlm would terminate the whole
        // process (thread entry without a handler). Malformed helper replies
        // (wrong JSON types) and callback throws land here - log and continue.
        try {
          if (j.row.actionType == "script") runScript(j.row, j.firedAt);
          else runLlm(j.row, "");
        } catch (const std::exception& ex) {
          if (logCb_) logCb_(std::string("(hooks) hook dispatch error: ") + ex.what());
        } catch (...) {
          if (logCb_) logCb_("(hooks) hook dispatch error: unknown exception");
        }
      }
      lk.lock();
    }
  }
}

void HookScheduler::collectDueHooks(std::vector<DispatchJob>& jobs) {
  if (!openDb()) return;
  // Budget window: reset when the date changes.
  const std::string today = nowIso().substr(0, 10);
  if (today != today_) {
    today_ = today;
    firedToday_ = 0;
  }
  const std::time_t now = std::time(nullptr);
  std::tm nowTm{};
  localtime_s(&nowTm, &now);
  const long long nowSecs = static_cast<long long>(now);
  const int nowMinute = nowTm.tm_hour * 60 + nowTm.tm_min;

  for (auto& h : loadHooks()) {
    if (!h.enabled || h.status != "active") continue;
    bool due = false;
    std::string last = h.lastFiredAt;
    if (h.triggerType == "interval" && h.intervalSeconds >= 60) {
      if (last.empty()) {
        due = true;
      } else {
        std::tm lastTm{};
        if (parseDateTime(last, &lastTm)) {
          const std::time_t lastT = std::mktime(&lastTm);
          due = (nowSecs - lastT) >= h.intervalSeconds;
        } else {
          due = true;
        }
      }
    } else if (h.triggerType == "at" && h.atHour >= 0) {
      const int targetMinute = h.atHour * 60 + h.atMinute;
      due = nowMinute == targetMinute;
      // Dedupe on the FULL timestamp (date included): comparing only
      // hour:minute would suppress every later day's fire (yesterday's
      // 12:34 matches today's 12:34) - the hook would fire exactly once ever.
      if (due && !last.empty()) {
        std::tm lastTm{};
        if (parseDateTime(last, &lastTm)) {
          due = nowTm.tm_year != lastTm.tm_year ||
                nowTm.tm_yday != lastTm.tm_yday ||
                nowTm.tm_hour != lastTm.tm_hour ||
                nowTm.tm_min != lastTm.tm_min;
        }
      }
    }
    if (!due) continue;

    std::string reason;
    if (!guardAllowFire(h, &reason)) {
      // An interval hook blocked by a guard (silent hours / budget) stays
      // "due" every tick - without throttling this logs the same line once
      // per second for the whole silent window. Log once per hook per minute.
      const long long nowMs = static_cast<long long>(nowSecs);
      auto it = skipLogState_.find(h.id);
      if (it == skipLogState_.end() || it->second.first != reason ||
          nowMs - it->second.second >= 60) {
        if (logCb_)
          logCb_("(hooks) skipped hook id=" + h.id + " reason=" + reason);
        skipLogState_[h.id] = {reason, nowMs};
      }
      continue;
    }
    ++firedToday_;
    const std::string firedAt = nowIso();
    updateFired(h.id, firedAt, "", h.status);
    if (logCb_)
      logCb_("(hooks) firing hook id=" + h.id + " name=" + h.name +
             " action=" + h.actionType + " intent=" + h.intent);
    jobs.push_back({h, firedAt});
  }
}

bool HookScheduler::guardAllowFire(const HookRow& h, std::string* reason) {
  if (firedToday_ >= cfg_.dailyBudget) {
    *reason = "daily budget reached (" + std::to_string(cfg_.dailyBudget) + ")";
    return false;
  }
  const std::time_t now = std::time(nullptr);
  std::tm nowTm{};
  localtime_s(&nowTm, &now);
  if (isSilentHour(nowTm.tm_hour)) {
    *reason = "silent hours (" + std::to_string(cfg_.silentStart) + "-" +
              std::to_string(cfg_.silentEnd) + ")";
    return false;
  }
  if (h.status == "broken") {
    *reason = "circuit breaker (previous failures)";
    return false;
  }
  return true;
}

// action_type=script: sandbox-only execution (no LLM), then offer the result
// to the LLM (interface #1) which decides whether to notify the user
// (interface #2).
void HookScheduler::runScript(const HookRow& h, const std::string& firedAt) {
  std::string scriptPath = h.scriptPath;
  if (scriptPath.empty()) {
    updateFired(h.id, firedAt, "empty script_path", "active");
    if (logCb_) logCb_("(hooks) hook id=" + h.id + " failed: empty script_path");
    return;
  }
  // White-list re-check (defense in depth; the same rules as manage(), incl.
  // cmd metacharacters - the path ends up in a cmd command line).
  const auto isPathChar = [](char c) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9');
    return alnum || c == '_' || c == '-' || c == '.' || c == '\\' ||
           c == '/' || c == ' ' || c == '(' || c == ')' || c == '@' ||
           c == '$';
  };
  bool okChars = true;
  for (const char c : scriptPath)
    if (!isPathChar(c)) { okChars = false; break; }
  if (scriptPath.find("..") != std::string::npos ||
      scriptPath.find(':') != std::string::npos ||
      scriptPath[0] == '/' || scriptPath[0] == '\\' || !okChars) {
    updateFired(h.id, firedAt, "script_path outside sandbox workspace", "active");
    if (logCb_)
      logCb_("(hooks) hook id=" + h.id + " rejected: script_path out of bounds");
    return;
  }
  // Execute via the existing sandbox channel (sandbox user, 30s, output cap).
  // cmd.exe misparses quoted relative paths containing forward slashes
  // ("cmd /c \"a/b.cmd\"" runs `a` as a command), so normalize separators.
  for (auto& ch : scriptPath)
    if (ch == '/') ch = '\\';
  std::string cmd = "\"" + scriptPath + "\"";
  const std::string ext = scriptPath.size() >= 3
                              ? scriptPath.substr(scriptPath.size() - 3)
                              : "";
  if (ext == ".py") cmd = "python \"" + scriptPath + "\"";
  const std::string reply = sandboxExecute(
      nlohmann::json{{"op", "bash"}, {"command", cmd}}.dump());
  auto j = nlohmann::json::parse(reply, nullptr, false);
  const std::string result = (!j.is_discarded() && j.is_object() &&
                              j.contains("ok") && j["ok"].get<bool>())
                                 ? j.value("output", "")
                                 : (j.is_discarded() ? reply : j.value("error", ""));
  if (logCb_)
    logCb_("(hooks) hook id=" + h.id + " script output len=" +
           std::to_string(result.size()));
  if (result.empty() || result.rfind("(sandbox", 0) == 0) {
    updateFired(h.id, firedAt, "script failed: " + result, "active");
    if (logCb_)
      logCb_("(hooks) hook id=" + h.id + " script failed: " + result);
    return;
  }
  updateFired(h.id, firedAt, "", "active");
  // Interface #1: offer the result to the LLM for judgement.
  if (llmCb_) {
    std::string judgment = llmCb_(
        h.intent.empty() ? "定时任务脚本执行完成，请根据结果决定是否需要提醒用户"
                         : h.intent,
        "脚本执行结果:\n" + result);
    // Interface #2: the LLM decides whether to notify the user; non-trivial
    // replies are surfaced on the panel.
    if (!judgment.empty() && notifyCb_) notifyCb_(judgment);
  }
}

// action_type=llm / llm_script: independent LLM orchestration.
void HookScheduler::runLlm(const HookRow& h, const std::string& context) {
  if (!llmCb_) {
    updateFired(h.id, nowIso(), "no LLM callback", "active");
    return;
  }
  std::string ctx = h.context;
  if (!context.empty()) ctx = context;
  const std::string out = llmCb_(
      h.intent.empty() ? "定时任务已触发" : h.intent, ctx);
  if (logCb_)
    logCb_("(hooks) hook id=" + h.id + " llm output len=" +
           std::to_string(out.size()));
  if (notifyCb_ && !out.empty()) notifyCb_(out);
}

} // namespace aoi
