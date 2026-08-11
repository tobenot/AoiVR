// hook_test: standalone verification for the hook scheduler (dedicated
// thread, interval/script firing, callbacks). Not part of the agent.
#include <cstdio>
#include <thread>

#include <sqlite3.h>

#include "hook_scheduler.hpp"

int main() {
  aoi::HookConfig cfg;
  cfg.maxHooks = 10;
  cfg.dailyBudget = 100;
  cfg.silentStart = 0;
  cfg.silentEnd = 0;  // no silent window for the test
  aoi::HookScheduler sched("hook_test.db", cfg);
  sched.setCallbacks(
      [](const std::string& intent, const std::string& context) -> std::string {
        printf("[LLM] intent=%s context_len=%zu\n", intent.c_str(),
               context.size());
        if (!context.empty())
          printf("[LLM] context=%.300s\n", context.c_str());
        return "测试提醒：hook 已触发";
      },
      [](const std::string& text) { printf("[NOTIFY] %s\n", text.c_str()); },
      [](const std::string& line) { printf("[LOG] %s\n", line.c_str()); });
  sched.start();

  // 1) create interval llm hook
  auto r = sched.manage({{"action", "create"},
                         {"name", "test-llm"},
                         {"trigger", {{"type", "interval"},
                                      {"interval_seconds", 60}}},
                         {"action_type", "llm"},
                         {"intent", "测试定时推理"}});
  printf("[CREATE-LLM] %s\n", r.value("content", "").c_str());

  // 2) create script hook
  r = sched.manage({{"action", "create"},
                    {"name", "test-script"},
                    {"trigger", {{"type", "interval"},
                                 {"interval_seconds", 60}}},
                    {"action_type", "script"},
                    {"intent", "测试脚本"},
                    {"script_path", "hook_scripts/hello.cmd"}});
  printf("[CREATE-SCRIPT] %s\n", r.value("content", "").c_str());

  // 3) malicious script_path must be rejected
  r = sched.manage({{"action", "create"},
                    {"name", "evil"},
                    {"trigger", {{"type", "interval"},
                                 {"interval_seconds", 60}}},
                    {"action_type", "script"},
                    {"script_path", "C:\\Windows\\evil.cmd"}});
  printf("[CREATE-EVIL] %s\n", r.value("content", "").c_str());

  // 4) list
  r = sched.manage({{"action", "list"}});
  printf("[LIST]\n%s\n", r.value("content", "").c_str());

  // 5) force both hooks due: set last_fired_at to the distant past
  {
    sqlite3* db = nullptr;
    if (sqlite3_open("hook_test.db", &db) == SQLITE_OK) {
      sqlite3_exec(db,
                   "UPDATE agent_hooks SET last_fired_at='2000-01-01T00:00:00'",
                   nullptr, nullptr, nullptr);
      sqlite3_close(db);
    }
  }

  // Wait for the scheduler to pick them up (1s tick).
  std::this_thread::sleep_for(std::chrono::seconds(3));
  sched.stop();

  // 6) cancel one
  r = sched.manage({{"action", "cancel"}, {"name", "test-script"}});
  printf("[CANCEL] %s\n", r.value("content", "").c_str());
  return 0;
}
