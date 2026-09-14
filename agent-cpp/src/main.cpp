#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

#include "agent.hpp"

namespace {
std::atomic<bool> g_running{true};
// Atomic: the CRT signal handler runs on another thread and reads this while
// main() may be clearing it during shutdown (a plain pointer would be a race).
std::atomic<aoi::AoiAgent*> g_agent{nullptr};

void onSignal(int) {
  printf("\n[Main] Shutting down...\n");
  g_running = false;
  if (auto* a = g_agent.load()) a->stop();
}
} // namespace

// Standalone debug mode: reads JSON messages from stdin (one per line) and
// feeds them to the agent via AoiAgent's in-process sendJson; outbound messages
// are printed to stdout as JSON. This mirrors what Unity does through the DLL:
//   echo '{"type":"user_input","payload":{"text":"你好"}}' | aoi-agent.exe
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so logs show immediately
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  aoi::AoiAgent agent;
  g_agent.store(&agent);

  // Route outbound messages to stdout so you can see what the agent would send
  // to Unity in the embedded (DLL) case.
  agent.setOutboundSink([](const std::string& json) {
    printf("<< %s\n", json.c_str());
  });

  if (!agent.start()) {
    printf("[Main] Failed to start\n");
    return 1;
  }
  printf("[Main] AOI Agent (C++) started\n");
  printf("[Main] Debug mode: feed JSON messages on stdin, e.g.\n");
  printf("[Main]   {\"type\":\"user_input\",\"payload\":{\"text\":\"你好\"}}\n");
  printf("[Main] Outbound messages print as '<< <json>'. Ctrl+C to stop.\n");

  std::string line;
  while (g_running && std::getline(std::cin, line)) {
    if (line.empty()) continue;
    // Trim whitespace.
    size_t b = 0, e = line.size();
    while (b < e && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) b++;
    while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) e--;
    const std::string msg = line.substr(b, e - b);
    if (msg.empty()) continue;
    if (msg == "quit" || msg == "exit") break;
    printf(">> %s\n", msg.c_str());
    if (!agent.sendJson(msg)) {
      printf("[Main] sendJson rejected (invalid JSON or not running)\n");
    }
    // Allow the agent's background thread to process before reading the next
    // line (and before EOF-exit in piped mode).
    Sleep(200);
  }

  // If stdin closed (piped mode), give in-flight processing a moment to finish
  // before we tear down.
  if (!std::cin.eof()) {
    g_running = false;
  } else {
    Sleep(3000);
  }
  agent.stop();
  g_agent.store(nullptr);
  printf("[Main] Exited\n");
  return 0;
}

