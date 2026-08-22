#include "agent_config.hpp"

#include <fstream>

#include <nlohmann/json.hpp>


namespace aoi {

namespace {

const std::string kDefaultLlmBaseUrl = "https://opencode.ai/zen/go/v1";
const std::string kDefaultLlmModel = "mimo-v2.5";
const std::string kDefaultTtsBaseUrl = "https://api.xiaomimimo.com/v1";
const std::string kDefaultTtsModel = "mimo-v2.5-tts";
const std::string kDefaultTtsVoice = "\xe5\x86\xb0\xe7\xb3\x96";  // 冰糖

std::string get(const nlohmann::json& obj, const char* key, const std::string& fallback) {
  if (obj.is_object() && obj.contains(key) && obj[key].is_string()) {
    const std::string v = obj[key].get<std::string>();
    if (!v.empty()) return v;
  }
  return fallback;
}

} // namespace

AgentFileConfig loadAgentConfig(const std::string& workDir) {
  AgentFileConfig cfg;
  cfg.llm.baseUrl = kDefaultLlmBaseUrl;
  cfg.llm.model = kDefaultLlmModel;
  cfg.llm.thinking = "disabled";  // no thinking by default (low latency)
  cfg.llm.reasoningEffort = "low";  // lowest reasoning strength by default
  cfg.tts.baseUrl = kDefaultTtsBaseUrl;
  cfg.tts.model = kDefaultTtsModel;
  cfg.tts.voice = kDefaultTtsVoice;

  std::ifstream f(workDir + "/aoi_config.json");
  if (!f.is_open()) return cfg;
  nlohmann::json root;
  try {
    // ignore_comments=true so aoi_config.json may carry "//" comments (the
    // example file documents optional fields this way). Valid JSON parses
    // exactly as before.
    root = nlohmann::json::parse(f, nullptr, true, true);
  } catch (...) {
    return cfg;
  }
  if (root.is_object() && root.contains("llm") && root["llm"].is_object()) {
    const auto& llm = root["llm"];
    cfg.llm.baseUrl = get(llm, "baseUrl", cfg.llm.baseUrl);
    cfg.llm.apiKey = get(llm, "apiKey", "");
    cfg.llm.model = get(llm, "model", cfg.llm.model);
    const std::string t = get(llm, "thinking", cfg.llm.thinking);
    if (t == "enabled" || t == "disabled" || t == "auto") cfg.llm.thinking = t;
    const std::string e = get(llm, "reasoningEffort", "");
    if (e.empty()) {
      // Explicit "" disables reasoning_effort (provider default) — do NOT
      // fall back to the built-in "low".
      cfg.llm.reasoningEffort = "";
    } else if (e == "low" || e == "medium" || e == "high") {
      cfg.llm.reasoningEffort = e;
    }
    // nativeAudio: bool (or "false"/"0"/"off" string, mirroring tts.enabled).
    if (llm.contains("nativeAudio")) {
      const auto& v = llm["nativeAudio"];
      if (v.is_boolean()) cfg.llm.nativeAudio = v.get<bool>();
      else if (v.is_string()) {
        const std::string s = v.get<std::string>();
        cfg.llm.nativeAudio = !(s == "false" || s == "0" || s == "off");
      }
    }
  }
  if (root.is_object() && root.contains("tts") && root["tts"].is_object()) {
    const auto& tts = root["tts"];
    if (tts.contains("enabled")) {
      if (tts["enabled"].is_boolean()) cfg.tts.enabled = tts["enabled"].get<bool>();
      else if (tts["enabled"].is_string()) {
        const std::string v = tts["enabled"].get<std::string>();
        if (v == "false" || v == "0" || v == "off") cfg.tts.enabled = false;
      }
    }
    cfg.tts.baseUrl = get(tts, "baseUrl", cfg.tts.baseUrl);
    cfg.tts.apiKey = get(tts, "apiKey", "");
    cfg.tts.model = get(tts, "model", cfg.tts.model);
    cfg.tts.voice = get(tts, "voice", cfg.tts.voice);
    // Optional; empty keeps the upstream single-voice behavior.
    cfg.tts.englishVoice = get(tts, "englishVoice", "");
  }
  cfg.knowledgeBase = get(root, "knowledgeBase", "");
  return cfg;
}

} // namespace aoi
