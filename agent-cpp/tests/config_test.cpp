#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent_config.hpp"

using namespace aoi;

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++failures;                                                          \
    }                                                                        \
  } while (0)

int main() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("aoi-agent-config-test-" + std::to_string(suffix));
  std::filesystem::create_directories(dir);

  {
    std::ofstream f(dir / "aoi_config.json");
    f << R"({
      "llm": {"baseUrl": "http://llm.test/v1", "apiKey": "llm-key", "model": "text-model"},
      "tts": {"baseUrl": "http://asr.test/v1", "apiKey": "tts-key"},
      "asr": {"baseUrl": "http://asr.test/v1", "model": "asr-model"},
      "hooks": {"enabled": true, "maxHooks": 0},
      "sandbox": {"read_dirs": ["docs", 42, ""]}
    })";
  }
  const auto sameOriginTts = loadAgentConfig(dir.string());
  CHECK(sameOriginTts.asr.baseUrl == "http://asr.test/v1");
  CHECK(sameOriginTts.asr.model == "asr-model");
  CHECK(sameOriginTts.asr.apiKey == "tts-key");
  CHECK(sameOriginTts.hooks.enabled);
  CHECK(sameOriginTts.hooks.maxHooks == 1);
  CHECK(sameOriginTts.sandboxReadDirs.size() == 1);
  CHECK(sameOriginTts.sandboxReadDirs[0] == "docs");

  {
    std::ofstream f(dir / "aoi_config.json");
    f << R"({
      "hooks": {
        "maxHooks": "not-an-integer",
        "dailyBudget": "not-an-integer",
        "scriptTimeoutSeconds": 9223372036854775807,
        "scriptOutputLimitBytes": "not-an-integer",
        "silentHours": {"start": "bad", "end": "bad"}
      },
      "sandbox": {"read_dirs": ["docs-after-bad-hooks"]}
    })";
  }
  const auto malformedHooks = loadAgentConfig(dir.string());
  CHECK(malformedHooks.hooks.maxHooks == 10);
  CHECK(malformedHooks.hooks.dailyBudget == 100);
  CHECK(malformedHooks.hooks.scriptTimeoutSeconds == 30);
  CHECK(malformedHooks.hooks.scriptOutputLimitBytes == 102400);
  CHECK(malformedHooks.hooks.silentStart == 0);
  CHECK(malformedHooks.hooks.silentEnd == 8);
  CHECK(malformedHooks.sandboxReadDirs.size() == 1);
  CHECK(malformedHooks.sandboxReadDirs[0] == "docs-after-bad-hooks");

  {
    std::ofstream f(dir / "aoi_config.json");
    f << R"({
      "llm": {"baseUrl": "http://llm.test/v1", "apiKey": "llm-key"},
      "tts": {"baseUrl": "http://tts.test/v1", "apiKey": "tts-key"},
      "asr": {"baseUrl": "http://asr.test/v1"}
    })";
  }
  const auto crossOrigin = loadAgentConfig(dir.string());
  CHECK(crossOrigin.asr.apiKey.empty());

  {
    std::ofstream f(dir / "aoi_config.json");
    f << R"({
      "llm": {"baseUrl": "http://asr.test/v1", "apiKey": "llm-key"},
      "tts": {"baseUrl": "http://tts.test/v1", "apiKey": "tts-key"},
      "asr": {"baseUrl": "http://asr.test/v1"}
    })";
  }
  const auto sameOriginLlm = loadAgentConfig(dir.string());
  CHECK(sameOriginLlm.asr.apiKey == "llm-key");

  {
    std::ofstream f(dir / "aoi_config.json");
    f << R"({
      "llm": {"apiKey": "llm-key"},
      "tts": {"apiKey": "tts-key"},
      "asr": {"apiKey": "asr-key"}
    })";
  }
  const auto explicitKey = loadAgentConfig(dir.string());
  CHECK(explicitKey.asr.apiKey == "asr-key");
  CHECK(explicitKey.asr.baseUrl == "https://api.xiaomimimo.com/v1");
  CHECK(explicitKey.asr.model == "mimo-v2.5");

  std::filesystem::remove_all(dir);
  if (failures == 0) {
    std::printf("ALL CONFIG TESTS PASSED\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
