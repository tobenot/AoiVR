// Dev tool: generate a real speech wav via MiMo TTS, for cache-lab audio
// experiments (real speech vs silence). Reads aoi_config.json "tts".
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "base64.hpp"
#include "nlohmann/json.hpp"
#include "tts.hpp"

int main(int argc, char** argv) {
  std::ifstream f("aoi_config.json");
  if (!f.is_open()) {
    std::cerr << "no aoi_config.json in cwd" << std::endl;
    return 1;
  }
  nlohmann::json cfg = nlohmann::json::parse(f, nullptr, false);
  if (cfg.is_discarded() || !cfg.contains("tts")) {
    std::cerr << "bad aoi_config.json" << std::endl;
    return 1;
  }
  const auto& t = cfg["tts"];
  aoi::TtsConfig tc;
  tc.apiKey = t.value("apiKey", "");
  tc.baseUrl = t.value("baseUrl", "");
  tc.model = t.value("model", "");
  tc.voice = t.value("voice", "");

  aoi::MiMoTTS tts(tc);
  const std::string text = argc > 1 ? argv[1]
                                    : "你好，这是模拟用户语音的一段测试音频。";
  std::vector<uint8_t> pcm;
  const bool ok = tts.speak(text, "", [&](const aoi::TtsChunk& c) {
    std::vector<uint8_t> dec;
    if (aoi::base64Decode(c.base64, dec))
      pcm.insert(pcm.end(), dec.begin(), dec.end());
  });
  if (!ok || pcm.empty()) {
    std::cerr << "TTS failed (ok=" << ok << " pcm=" << pcm.size() << ")"
              << std::endl;
    return 1;
  }
  const uint32_t rate = static_cast<uint32_t>(tts.sampleRate());
  const uint32_t dataSize = static_cast<uint32_t>(pcm.size());
  std::vector<uint8_t> wav(44 + dataSize, 0);
  wav[0] = 'R'; wav[1] = 'I'; wav[2] = 'F'; wav[3] = 'F';
  const uint32_t riffSize = 36 + dataSize;
  for (int i = 0; i < 4; ++i) wav[4 + i] = static_cast<uint8_t>(riffSize >> (8 * i));
  wav[8] = 'W'; wav[9] = 'A'; wav[10] = 'V'; wav[11] = 'E';
  wav[12] = 'f'; wav[13] = 'm'; wav[14] = 't'; wav[15] = ' ';
  wav[16] = 16;
  wav[20] = 1;
  wav[22] = 1;
  for (int i = 0; i < 4; ++i) wav[24 + i] = static_cast<uint8_t>(rate >> (8 * i));
  const uint32_t byteRate = rate * 2;
  for (int i = 0; i < 4; ++i) wav[28 + i] = static_cast<uint8_t>(byteRate >> (8 * i));
  wav[32] = 2;
  wav[34] = 16;
  wav[36] = 'd'; wav[37] = 'a'; wav[38] = 't'; wav[39] = 'a';
  for (int i = 0; i < 4; ++i) wav[40 + i] = static_cast<uint8_t>(dataSize >> (8 * i));
  std::memcpy(wav.data() + 44, pcm.data(), pcm.size());

  std::ofstream out("speech.wav", std::ios::binary);
  out.write(reinterpret_cast<const char*>(wav.data()), wav.size());
  out.close();
  std::cout << "speech.wav: rate=" << rate << " pcm=" << pcm.size()
            << " bytes, duration=" << (pcm.size() / 2 / rate) << "s" << std::endl;
  return 0;
}
