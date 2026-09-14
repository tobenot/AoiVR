#include "agent_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>


namespace aoi {

namespace {

const std::string kDefaultLlmBaseUrl = "https://opencode.ai/zen/go/v1";
const std::string kDefaultLlmModel = "mimo-v2.5";
const std::string kDefaultAsrBaseUrl = "https://api.xiaomimimo.com/v1";
const std::string kDefaultAsrModel = "mimo-v2.5";
const std::string kDefaultTtsBaseUrl = "https://api.xiaomimimo.com/v1";
const std::string kDefaultTtsModel = "mimo-v2.5-tts";
const std::string kDefaultTtsVoice = "\xe5\x86\xb0\xe7\xb3\x96";  // 冰糖

std::string originOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos || schemeEnd == 0) return {};
  const size_t authorityStart = schemeEnd + 3;
  const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
  std::string authority = url.substr(
      authorityStart, authorityEnd == std::string::npos ? std::string::npos
                                                          : authorityEnd - authorityStart);
  const size_t userInfo = authority.rfind('@');
  if (userInfo != std::string::npos) authority.erase(0, userInfo + 1);
  std::string scheme = url.substr(0, schemeEnd);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  std::transform(authority.begin(), authority.end(), authority.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (authority.empty()) return {};
  return scheme + "://" + authority;
}

bool sameOrigin(const std::string& lhs, const std::string& rhs) {
  const std::string left = originOf(lhs);
  const std::string right = originOf(rhs);
  return !left.empty() && left == right;
}

std::string get(const nlohmann::json& obj, const char* key, const std::string& fallback) {
  if (obj.is_object() && obj.contains(key) && obj[key].is_string()) {
    const std::string v = obj[key].get<std::string>();
    if (!v.empty()) return v;
  }
  return fallback;
}

int integerField(const nlohmann::json& obj, const char* key, int fallback) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number_integer())
    return fallback;
  try {
    return obj[key].get<int>();
  } catch (...) {
    // Out-of-range JSON integers must not abort parsing of later sections.
    return fallback;
  }
}

} // namespace

AgentFileConfig loadAgentConfig(const std::string& workDir) {
  AgentFileConfig cfg;
  cfg.llm.baseUrl = kDefaultLlmBaseUrl;
  cfg.llm.model = kDefaultLlmModel;
  cfg.llm.thinking = "disabled";  // no thinking by default (low latency)
  cfg.llm.reasoningEffort = "low";  // lowest reasoning strength by default
  cfg.asr.baseUrl = kDefaultAsrBaseUrl;
  cfg.asr.model = kDefaultAsrModel;
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
  if (root.is_object() && root.contains("asr") && root["asr"].is_object()) {
    const auto& asr = root["asr"];
    cfg.asr.baseUrl = get(asr, "baseUrl", cfg.asr.baseUrl);
    cfg.asr.apiKey = get(asr, "apiKey", "");
    cfg.asr.model = get(asr, "model", cfg.asr.model);
  }
  // Parse optional sections field-by-field. A malformed value must fall back
  // locally and must not prevent later safety settings (such as
  // sandbox.read_dirs) from being loaded.
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
  // Reuse a credential only across the same normalized origin. A TTS key may
  // fall back to ASR when both endpoints are the same provider; cross-provider
  // reuse is forbidden and requires an explicit asr.apiKey.
  if (cfg.asr.apiKey.empty()) {
    if (sameOrigin(cfg.asr.baseUrl, cfg.tts.baseUrl) && !cfg.tts.apiKey.empty()) {
      cfg.asr.apiKey = cfg.tts.apiKey;
    } else if (sameOrigin(cfg.asr.baseUrl, cfg.llm.baseUrl) && !cfg.llm.apiKey.empty()) {
      cfg.asr.apiKey = cfg.llm.apiKey;
    }
  }
  cfg.knowledgeBase = get(root, "knowledgeBase", "");
  cfg.vrcxDbPath = get(root, "vrcxDbPath", "");
  if (root.is_object() && root.contains("hooks") && root["hooks"].is_object()) {
    const auto& h = root["hooks"];
    if (h.contains("enabled")) {
      if (h["enabled"].is_boolean()) cfg.hooks.enabled = h["enabled"].get<bool>();
      else if (h["enabled"].is_string()) {
        const std::string v = h["enabled"].get<std::string>();
        if (v == "false" || v == "0" || v == "off") cfg.hooks.enabled = false;
        else if (v == "true" || v == "1" || v == "on") cfg.hooks.enabled = true;
      }
    }
    cfg.hooks.maxHooks = integerField(h, "maxHooks", cfg.hooks.maxHooks);
    cfg.hooks.dailyBudget = integerField(h, "dailyBudget", cfg.hooks.dailyBudget);
    if (h.contains("silentHours") && h["silentHours"].is_object()) {
      const auto& silentHours = h["silentHours"];
      cfg.hooks.silentStart =
          integerField(silentHours, "start", cfg.hooks.silentStart);
      cfg.hooks.silentEnd =
          integerField(silentHours, "end", cfg.hooks.silentEnd);
    }
    cfg.hooks.scriptTimeoutSeconds = integerField(
        h, "scriptTimeoutSeconds", cfg.hooks.scriptTimeoutSeconds);
    cfg.hooks.scriptOutputLimitBytes = integerField(
        h, "scriptOutputLimitBytes", cfg.hooks.scriptOutputLimitBytes);
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
  if (root.is_object() && root.contains("sandbox") && root["sandbox"].is_object()) {
    const auto& sandbox = root["sandbox"];
    if (sandbox.contains("read_dirs") && sandbox["read_dirs"].is_array()) {
      for (const auto& item : sandbox["read_dirs"]) {
        if (item.is_string()) {
          const std::string path = item.get<std::string>();
          if (!path.empty()) cfg.sandboxReadDirs.push_back(path);
        }
      }
    }
  }
  return cfg;
}

} // namespace aoi
