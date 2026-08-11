#include "aoi_agent_api.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "agent.hpp"

namespace {

std::atomic<bool> g_running{false};
std::thread g_agentThread;
std::shared_ptr<aoi::AoiAgent> g_agent;

// Serializes the whole lifecycle: Start/Stop must not race each other or a
// concurrent SendJson that reads g_agent (which agentWorker() resets on stop).
std::mutex g_lifecycleMutex;

std::mutex g_cbMutex;
AoiAgent_MessageCallback g_callback = nullptr;

std::mutex g_logMutex;
AoiAgent_LogCallback g_logCallback = nullptr;

// The agent's outbound sink: forward JSON to the registered C callback.
// Runs on the agent's background thread.
void forwardOutbound(const std::string& json) {
  AoiAgent_MessageCallback cb = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    cb = g_callback;
  }
  if (cb) cb(json.c_str());
}

// The agent's log sink: forward log lines to the registered C callback.
void forwardLog(const std::string& line) {
  AoiAgent_LogCallback cb = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_logMutex);
    cb = g_logCallback;
  }
  if (cb) cb(line.c_str());
}

// Worker: creates the agent, wires the in-process transport, starts it, and
// keeps it alive until Stop() flips the flag.
void agentWorker() {
  try {
    std::shared_ptr<aoi::AoiAgent> agent;
    {
      std::lock_guard<std::mutex> lk(g_lifecycleMutex);
      agent = g_agent = std::make_shared<aoi::AoiAgent>();
    }
    agent->setOutboundSink(forwardOutbound);
    agent->setLogSink(forwardLog);
    if (!agent->start()) {
      g_running = false;
      return;
    }
    while (g_running) {
      Sleep(200);
    }
    agent->stop();
    g_agent.reset();
  } catch (...) {
    // Any exception escaping a std::thread body is std::terminate (process
    // death). Catch it, clean up, and flip running off so the C ABI never
    // observes a half-built agent.
    if (g_agent) {
      try { g_agent->stop(); } catch (...) {}
      g_agent.reset();
    }
    g_running = false;
  }
}

} // namespace

extern "C" {

// Sets a process environment variable. SAFE ONLY while the agent is NOT
// running: MSVC's _putenv_s is not thread-safe and racing the agent's getenv
// reads is UB. Runtime SetEnv calls are ignored to keep the contract honest.
void AoiAgent_SetEnv(const char* name, const char* value) {
  if (!name) return;
  try {
    if (g_running.load()) return;  // ignore while the agent runs
    _putenv_s(name, value ? value : "");
  } catch (...) {
  }
}

int AoiAgent_Start(void) {
  try {
    std::lock_guard<std::mutex> lk(g_lifecycleMutex);
    if (g_running.load()) return 1;
    if (g_agentThread.joinable()) g_agentThread.join();
    try {
      g_agentThread = std::thread(agentWorker);
    } catch (...) {
      return -1;  // thread construction failed: leave g_running false
    }
    g_running = true;
    return 0;
  } catch (...) {
    return -1;
  }
}

void AoiAgent_Stop(void) {
  try {
    // Join OUTSIDE the lifecycle lock: agentWorker() takes g_lifecycleMutex
    // during agent construction, so joining while holding the lock can
    // deadlock on a Start->Stop race (worker waits for the lock, Stop waits
    // for the worker). Move the thread out under the lock, then join freely.
    std::thread t;
    {
      std::lock_guard<std::mutex> lk(g_lifecycleMutex);
      if (!g_running.exchange(false)) {
        t = std::move(g_agentThread);
      } else {
        t = std::move(g_agentThread);
      }
    }
    if (t.joinable()) t.join();
  } catch (...) {
  }
}

int AoiAgent_IsRunning(void) {
  try {
    return g_running.load() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

void AoiAgent_SetMessageCallback(AoiAgent_MessageCallback cb) {
  try {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    g_callback = cb;
  } catch (...) {
  }
}

void AoiAgent_SetLogCallback(AoiAgent_LogCallback cb) {
  try {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logCallback = cb;
  } catch (...) {
  }
}

int AoiAgent_SendJson(const char* json) {
  try {
    std::shared_ptr<aoi::AoiAgent> agent;
    {
      std::lock_guard<std::mutex> lk(g_lifecycleMutex);
      if (!json || !g_running.load() || !g_agent) return 0;
      agent = g_agent;  // copy the shared_ptr: the object stays alive even if
                        // agentWorker resets g_agent concurrently on stop.
    }
    return agent->sendJson(json) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

namespace {

// Build a message envelope and push it into the agent.
int sendTypedMessage(const char* type, const nlohmann::json& payload, const char* id) {
  if (!type) return 0;
  // Build the full envelope JSON here (schema lives in the DLL). Field names
  // may be encrypted by an external build layer for shipped builds; the wire
  // schema stays the same either way. JSON is built with nlohmann so paths /
  // strings with quotes or backslashes are always escaped.
  const std::string msg =
      nlohmann::json{{"type", type},
                     {"payload", payload},
                     {"timestamp", ""},
                     {"id", id ? id : ""}}
          .dump();
  try {
    std::shared_ptr<aoi::AoiAgent> agent;
    {
      std::lock_guard<std::mutex> lk(g_lifecycleMutex);
      if (!g_running.load() || !g_agent) return 0;
      agent = g_agent;  // copy the shared_ptr (see SendJson comment)
    }
    return agent->sendJson(msg) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

} // namespace

int AoiAgent_SendStateChange(const char* state, const char* mode, const char* shotPath) {
  try {
    nlohmann::json payload{{"state", state ? state : ""}};
    if (mode && *mode) payload["mode"] = mode;
    if (shotPath && *shotPath) payload["shot_path"] = shotPath;
    return sendTypedMessage("state_change", payload, nullptr);
  } catch (...) {
    return 0;
  }
}

int AoiAgent_SendTtsStop(void) {
  try {
    return sendTypedMessage("tts_stop", nlohmann::json::object(), nullptr);
  } catch (...) {
    return 0;
  }
}

int AoiAgent_SendScreenshotPath(const char* requestId, const char* path) {
  try {
    return sendTypedMessage("screenshot_response",
                            nlohmann::json{{"path", path ? path : ""}}, requestId);
  } catch (...) {
    return 0;
  }
}

int AoiAgent_SendScreenshotImage(const char* requestId, const char* base64Jpeg) {
  try {
    return sendTypedMessage(
        "screenshot_response",
        nlohmann::json{{"image", base64Jpeg ? base64Jpeg : ""},
                       {"width", 1024},
                       {"height", 1024},
                       {"format", "jpeg"}},
        requestId);
  } catch (...) {
    return 0;
  }
}

int AoiAgent_SendScreenshotError(const char* requestId, const char* error) {
  try {
    return sendTypedMessage(
        "screenshot_response",
        nlohmann::json{{"image", ""},
                       {"width", 0},
                       {"height", 0},
                       {"format", "png"},
                       {"error", error ? error : "screenshot_failed"}},
        requestId);
  } catch (...) {
    return 0;
  }
}

int AoiAgent_SendVrSkillResult(const char* requestId, const char* resultJson) {
  try {
    nlohmann::json payload = nlohmann::json::object();
    if (resultJson && *resultJson) {
      const nlohmann::json parsed = nlohmann::json::parse(resultJson, nullptr, false);
      if (!parsed.is_discarded()) payload = parsed;
    }
    return sendTypedMessage("vr_skill_response", payload, requestId);
  } catch (...) {
    return 0;
  }
}

int AoiAgent_SendDisplayResult(bool success) {
  try {
    return sendTypedMessage("display_result",
                            nlohmann::json{{"success", success}}, nullptr);
  } catch (...) {
    return 0;
  }
}

} // extern "C"
