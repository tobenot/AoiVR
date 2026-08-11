#pragma once
#include <string>

namespace aoi {

// LLM provider settings (aoi_config.json -> "llm").
struct LlmConfig {
  std::string baseUrl;
  std::string apiKey;
  std::string model;
  // "enabled" | "disabled" | "auto" -> "thinking": {"type": ...} on the wire.
  std::string thinking = "auto";
  // Optional "low" | "medium" | "high" -> "reasoning_effort" on the wire.
  std::string reasoningEffort;
};

// TTS settings (aoi_config.json -> "tts").
struct TtsSettings {
  bool enabled = true;  // master switch; false disables auto speech entirely
  std::string baseUrl;
  std::string apiKey;
  std::string model;
  std::string voice;
};

// Runtime configuration loaded from aoi_config.json next to the executable.
// This file FULLY replaces the old .env / models.json mechanism: missing
// fields fall back to the built-in defaults below (never to environment
// variables).
struct AgentFileConfig {
  LlmConfig llm;
  TtsSettings tts;
  // Optional custom path to the VRCX SQLite database (aoi_config.json top-level
  // "vrcxDbPath"). Empty -> default %APPDATA%\VRCX\VRCX.sqlite3.
  std::string vrcxDbPath;
  // Timer-hook scheduler guardrails (aoi_config.json -> "hooks").
  struct {
    bool enabled = true;
    int maxHooks = 10;
    int dailyBudget = 100;
    int silentStart = 0;  // hour, inclusive
    int silentEnd = 8;    // hour, exclusive
    int scriptTimeoutSeconds = 30;
    int scriptOutputLimitBytes = 102400;
  } hooks;
};

// Load aoi_config.json from `workDir`. Missing file / unparsable JSON / missing
// fields all fall back to defaults. Never throws.
AgentFileConfig loadAgentConfig(const std::string& workDir);

} // namespace aoi
