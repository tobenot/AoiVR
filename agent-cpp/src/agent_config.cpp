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
    root = nlohmann::json::parse(f);
  } catch (...) {
    return cfg;
  }
  if (root.is_object() && root.contains("llm") && root["llm"].is_object()) {
    const auto& llm = root["llm"];
    cfg.llm.baseUrl = get(llm, "baseUrl", cfg.llm.baseUrl);
    cfg.llm.apiKey = get(llm, "apiKey", "");
    cfg.llm.model = get(llm, "model", cfg.llm.model);
    // history.json persistence is OFF by default (clean history per launch);
    // opt in with "persistHistory": true.
    if (llm.contains("persistHistory") && llm["persistHistory"].is_boolean())
      cfg.llm.persistHistory = llm["persistHistory"].get<bool>();
    const std::string t = get(llm, "thinking", cfg.llm.thinking);
    if (t == "enabled" || t == "disabled" || t == "auto") cfg.llm.thinking = t;
    // reasoningEffort: only overridden when the key is EXPLICITLY present.
    // An explicit "" disables reasoning_effort (provider default); an ABSENT
    // key keeps the built-in default - get() cannot distinguish the two.
    if (llm.contains("reasoningEffort")) {
      const std::string e = llm["reasoningEffort"].is_string()
                                ? llm["reasoningEffort"].get<std::string>()
                                : "";
      if (e.empty()) {
        cfg.llm.reasoningEffort = "";
      } else if (e == "low" || e == "medium" || e == "high") {
        cfg.llm.reasoningEffort = e;
      }
    }
  }
  // Any type-mismatched value below (e.g. "maxHooks":"abc") would throw a
  // nlohmann type_error out of loadAgentConfig and kill agent startup.
  // Malformed config fields degrade to defaults instead.
  try {
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
  }
  cfg.vrcxDbPath = get(root, "vrcxDbPath", "");
  if (root.is_object() && root.contains("hooks") && root["hooks"].is_object()) {
    const auto& h = root["hooks"];
    if (h.contains("enabled")) {
      if (h["enabled"].is_boolean()) cfg.hooks.enabled = h["enabled"].get<bool>();
      else if (h["enabled"].is_string()) {
        const std::string v = h["enabled"].get<std::string>();
        if (v == "false" || v == "0" || v == "off") cfg.hooks.enabled = false;
      }
    }
    cfg.hooks.maxHooks = h.value("maxHooks", cfg.hooks.maxHooks);
    cfg.hooks.dailyBudget = h.value("dailyBudget", cfg.hooks.dailyBudget);
    if (h.contains("silentHours") && h["silentHours"].is_object()) {
      cfg.hooks.silentStart = h["silentHours"].value("start", cfg.hooks.silentStart);
      cfg.hooks.silentEnd = h["silentHours"].value("end", cfg.hooks.silentEnd);
    }
    cfg.hooks.scriptTimeoutSeconds =
        h.value("scriptTimeoutSeconds", cfg.hooks.scriptTimeoutSeconds);
    cfg.hooks.scriptOutputLimitBytes =
        h.value("scriptOutputLimitBytes", cfg.hooks.scriptOutputLimitBytes);
    // Range-clamp: invalid values would silently disable ALL hooks
    // (dailyBudget<=0 makes every fire skipped, silentEnd>23 silences
    // everything, maxHooks<=0 rejects every create).
    if (cfg.hooks.maxHooks < 1) cfg.hooks.maxHooks = 1;
    if (cfg.hooks.dailyBudget < 1) cfg.hooks.dailyBudget = 1;
    cfg.hooks.silentStart = (cfg.hooks.silentStart + 24) % 24;
    cfg.hooks.silentEnd = (cfg.hooks.silentEnd + 24) % 24;
    if (cfg.hooks.scriptTimeoutSeconds < 1) cfg.hooks.scriptTimeoutSeconds = 30;
    if (cfg.hooks.scriptOutputLimitBytes < 1024)
      cfg.hooks.scriptOutputLimitBytes = 30 * 1024;
  }
  } catch (...) {
    // A malformed field type must not abort startup; keep what parsed so far.
  }
  return cfg;
}

} // namespace aoi
