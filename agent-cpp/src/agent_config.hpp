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
  // When true, the user's voice is sent to the model as a native input_audio
  // content block (the model "hears" tone/emphasis; requires an endpoint that
  // supports OpenAI input_audio, e.g. MiMo direct). When false (default), the
  // voice is transcribed locally (sherpa-onnx, same engine as interpretation)
  // and ONLY the transcript is sent — works on every OpenAI-compatible
  // endpoint. A HTTP 400 that looks like an input_audio rejection auto-falls
  // back to the transcript either way (see llm_client.cpp).
  bool nativeAudio = true;
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
  TtsSettings tts;
  // Optional path to a private knowledge base file (UTF-8, one
  // "term | explanation" per line, pipe separated). Injected into the system
  // prompt when the file exists and is non-empty; empty = feature off.
  // Default behavior (no file) is identical to upstream.
  std::string knowledgeBase;
};

// Load aoi_config.json from `workDir`. Missing file / unparsable JSON / missing
// fields all fall back to defaults. Never throws.
AgentFileConfig loadAgentConfig(const std::string& workDir);

} // namespace aoi
