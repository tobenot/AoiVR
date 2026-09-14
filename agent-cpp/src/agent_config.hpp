#pragma once
#include <string>
#include <vector>

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
  // When true, the user's voice is sent to the model as a native input_audio
  // content block (the model "hears" tone/emphasis; requires an endpoint that
  // supports OpenAI input_audio, e.g. MiMo direct). When false, the voice is
  // sent to the configured remote ASR first and ONLY the transcript reaches
  // the LLM — this works with text-only OpenAI-compatible endpoints. A HTTP
  // 400 that looks like an input_audio rejection also switches to remote ASR
  // on later turns (see llm_client.cpp).
  bool nativeAudio = true;
  // Load/persist conversation history to history.json next to the exe.
  // Default OFF: each launch starts with a clean in-memory history (old
  // sessions are not injected - they can mislead the model with stale
  // conclusions, e.g. obsolete tool-capability claims).
  bool persistHistory = false;
};

// Remote speech-to-text settings (aoi_config.json -> "asr"). The endpoint
// receives an OpenAI input_audio chat request and returns plain transcript
// text. The API key is filled by loadAgentConfig with an explicit asr.apiKey
// preferred; otherwise TTS/LLM keys are considered only when their normalized
// origin matches the ASR origin.
struct AsrConfig {
  std::string baseUrl = "https://api.xiaomimimo.com/v1";
  std::string apiKey;
  std::string model = "mimo-v2.5";
};

// TTS settings (aoi_config.json -> "tts").
struct TtsSettings {
  bool enabled = true;  // master switch; false disables auto speech entirely
  std::string baseUrl;
  std::string apiKey;
  std::string model;
  std::string voice;
  // Optional English voice (e.g. "Mia"); empty disables per-language voice
  // selection and keeps the upstream single-voice behavior.
  std::string englishVoice;
};

// Runtime configuration loaded from aoi_config.json next to the executable.
// This file FULLY replaces the old .env / models.json mechanism: missing
// fields fall back to the built-in defaults below (never to environment
// variables).
struct AgentFileConfig {
  LlmConfig llm;
  AsrConfig asr;
  TtsSettings tts;
  // Optional path to a private knowledge base file (UTF-8, one
  // "term | explanation" per line, pipe separated). Injected into the system
  // prompt when the file exists and is non-empty; empty = feature off.
  // Default behavior (no file) is identical to upstream.
  std::string knowledgeBase;
  // Optional custom path to the VRCX SQLite database (aoi_config.json top-level
  // "vrcxDbPath"). Empty -> default %APPDATA%\VRCX\VRCX.sqlite3.
  std::string vrcxDbPath;
  // Timer-hook scheduler guardrails (aoi_config.json -> "hooks").
  struct {
    bool enabled = false;
    int maxHooks = 10;
    int dailyBudget = 100;
    int silentStart = 0;  // hour, inclusive
    int silentEnd = 8;    // hour, exclusive
    int scriptTimeoutSeconds = 30;
    int scriptOutputLimitBytes = 102400;
  } hooks;
  // Directories the sandbox user is granted READ+EXECUTE on at startup
  // (aoi_config.json -> "sandbox" -> "read_dirs"). Relative entries resolve
  // against the agent exe dir (e.g. "docs" -> <exeDir>\docs). Registered once
  // per launch so the model can read shipped knowledge files that live outside
  // the sandbox workspace. Never include aoi_config.json's directory itself:
  // Never add the directory containing aoi_config.json here until the
  // Windows ACL wiring has validated the final resolved path.
  std::vector<std::string> sandboxReadDirs;
};

// Load aoi_config.json from `workDir`. Missing file / unparsable JSON / missing
// fields all fall back to defaults. Never throws.
AgentFileConfig loadAgentConfig(const std::string& workDir);

} // namespace aoi
