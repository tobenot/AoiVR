#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

struct sqlite3;

namespace aoi {

// Guardrails for hook firing (code-level; never rely on the model).
struct HookConfig {
  int maxHooks = 10;
  int dailyBudget = 100;
  int silentStart = 0;   // hour, inclusive
  int silentEnd = 8;     // hour, exclusive
  int scriptTimeoutSeconds = 30;      // informational: sandboxExecute has its own
  size_t scriptOutputLimitBytes = 102400;  // informational: helper caps output
};

// Generic timer-hook scheduler. Runs on its OWN thread (never blocks the
// message loop / main thread):
//
//   scheduler thread: check due hooks -> guardrails -> dispatch (serial)
//     - action_type=llm        : independent LlmSession inference (no history)
//     - action_type=script     : sandboxExecute as the sandbox user, then the
//                                result is offered to the LLM (interface #1)
//                                which decides whether to notify the user
//                                (interface #2)
//     - action_type=llm_script : LLM inference; the model calls the script tool
//                                itself through the normal tool loop
//
// Hooks persist in SQLite (<workdir>/agent.db, agent_hooks table) so they
// survive restarts. All public methods are thread-safe (mutex).
class HookScheduler {
 public:
  // LLM orchestration callback: runs an INDEPENDENT inference (own session,
  // own system prompt incl. AGENTS.md, full tool set), returns the final text.
  // Implemented by the agent; invoked on the scheduler thread only.
  using LlmCallback = std::function<std::string(const std::string& intent,
                                                const std::string& context)>;
  // Notify the user (panel message). Thread-safe sink provided by the agent.
  using NotifyCallback = std::function<void(const std::string& text)>;
  // Audit log sink (debug/logLine).
  using LogCallback = std::function<void(const std::string& line)>;

  HookScheduler(const std::string& dbPath, HookConfig cfg);
  ~HookScheduler();
  HookScheduler(const HookScheduler&) = delete;
  HookScheduler& operator=(const HookScheduler&) = delete;

  void setCallbacks(LlmCallback llm, NotifyCallback notify, LogCallback log);

  void start();
  void stop();

  // hook_manage tool entry (create/list/cancel/pause/resume). Thread-safe.
  nlohmann::json manage(const nlohmann::json& args);

 private:
  struct HookRow {
    std::string id, name, triggerType, actionType, intent, scriptPath,
        context, status, lastFiredAt, lastError;
    long long intervalSeconds = 0;
    int atHour = -1, atMinute = -1;
    bool enabled = false;
  };

  bool openDb();
  void ensureSchema();
  std::vector<HookRow> loadHooks();
  bool saveHook(const HookRow& h);
  bool updateFired(const std::string& id, const std::string& firedAt,
                   const std::string& error, const std::string& status);

  void schedulerLoop();
  // Collect due hooks while holding mtx_ (fired state updated here); the
  // actual dispatch happens OUTSIDE the lock - callbacks may re-enter
  // manage() which takes mtx_.
  struct DispatchJob {
    HookRow row;
    std::string firedAt;
  };
  void collectDueHooks(std::vector<DispatchJob>& jobs);
  bool guardAllowFire(const HookRow& h, std::string* reason);
  void runScript(const HookRow& h, const std::string& firedAt);
  void runLlm(const HookRow& h, const std::string& context);

  bool isSilentHour(int hour) const;
  std::string nowIso() const;

  std::string dbPath_;
  HookConfig cfg_;
  sqlite3* db_ = nullptr;
  std::mutex mtx_;                 // guards db_ and all state
  std::condition_variable cv_;
  std::thread thread_;
  bool running_ = false;
  std::string today_;              // YYYY-MM-DD of the current budget window
  int firedToday_ = 0;             // fires within the current budget window
  LlmCallback llmCb_;
  NotifyCallback notifyCb_;
  LogCallback logCb_;
};

} // namespace aoi
