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
      "tts": {"apiKey": "tts-key"},
      "asr": {"baseUrl": "http://asr.test/v1", "model": "asr-model"}
    })";
  }
  const auto fallback = loadAgentConfig(dir.string());
  CHECK(fallback.asr.baseUrl == "http://asr.test/v1");
  CHECK(fallback.asr.model == "asr-model");
  CHECK(fallback.asr.apiKey == "tts-key");

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
