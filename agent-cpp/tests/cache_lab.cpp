// Cache-lab: measures the prompt-cache hit sequence of LlmSession across
// consecutive turns, each with a LONG input (>=256 tokens) so the per-turn
// delta is well above any minimum cache granularity. Detects "every-other
// turn miss" patterns (prefix instability) without the audio-folding
// variable.
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "llm_client.hpp"
#include "nlohmann/json.hpp"

namespace {

// ~400-500 chars of Chinese per turn (~300+ tokens). Turn-specific topic so
// each new message is genuinely new content, padded to the length target.
std::string longText(int turn) {
  const char* topics[] = {
      "机器学习的梯度下降优化算法原理",
      "中国高铁网络的规划与建设历程",
      "量子计算对密码学的影响",
      "长江流域的生态系统多样性",
      "人工神经网络的反向传播机制",
      "太阳能光伏发电的技术发展",
      "深海探测技术与海洋资源",
      "城市轨道交通的客流预测方法",
  };
  const std::string topic = topics[turn % 8];
  // Fill: a stable paragraph repeated with slight variation keeps the turn
  // long; the fill itself is part of the new message (new prefix content).
  std::string t = "请详细讲解" + topic + "。要求：";
  t += "1) 先给出背景介绍；2) 再展开核心原理；3) 最后给出实际应用场景。";
  // Target >= 1024 tokens (~4000+ chars with MiMo's ~4 chars/token CJK
  // tokenizer): the first turn must itself exceed the provider's minimum
  // cacheable prefix, or no cache is created and turn 2 misses everything.
  while (t.size() < 4500) {
    t += "同时请结合历史发展脉络、关键技术突破以及当前研究前沿，"
         "从多个角度进行深入而全面的分析说明，确保内容充实、层次分明。";
  }
  return t;
}

} // namespace

int main(int argc, char** argv) {
  std::ifstream f("aoi_config.json");
  if (!f.is_open()) {
    std::cerr << "no aoi_config.json in cwd" << std::endl;
    return 1;
  }
  nlohmann::json cfgj = nlohmann::json::parse(f, nullptr, false);
  if (cfgj.is_discarded() || !cfgj.contains("llm")) {
    std::cerr << "bad aoi_config.json" << std::endl;
    return 1;
  }
  const auto& llm = cfgj["llm"];

  const bool withAudio = argc > 2 && std::string(argv[2]) == "--audio";

  aoi::LlmSession::Config cfg;
  cfg.baseUrl = llm.value("baseUrl", "");
  cfg.apiKey = llm.value("apiKey", "");
  cfg.modelId = llm.value("model", "");
  cfg.thinking = "disabled";
  cfg.reasoningEffort = "";
  cfg.systemPrompt = "You are a helpful assistant. Answer concisely in Chinese.";

  aoi::LlmSession session(cfg);
  session.setLogSink([](const std::string& line) {
    if (line.find("usage prompt=") != std::string::npos) {
      const size_t p = line.find("usage");
      std::cout << "[USAGE] " << line.substr(p) << std::endl;
    }
  });

  // TTFT slot: subscribed ONCE, shared state (heap) so the listener never
  // dangles across turns (a per-turn local captured by reference would be UB).
  auto ttftSlot = std::make_shared<long long>(-1);
  auto t0slot = std::make_shared<std::chrono::steady_clock::time_point>();
  session.subscribe([ttftSlot, t0slot](const aoi::SessionEvent& e) {
    if (e.type == "message_update" && *ttftSlot < 0) {
      *ttftSlot = static_cast<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - *t0slot)
              .count());
    }
  });

  const int turns = argc > 1 ? std::atoi(argv[1]) : 8;
  for (int i = 1; i <= turns; ++i) {
    const std::string text = longText(i);
    std::vector<aoi::ContentPart> parts;
    // Reset the TTFT slot for this turn.
    *ttftSlot = -1;
    *t0slot = std::chrono::steady_clock::now();
    if (withAudio) {
      // Valid 16-bit PCM mono wav: header + silence. Half-size payload
      // (8k-equivalent sample count) to verify cache-miss scales with the
      // audio base64 size (16k audio -> ~580 token miss per turn; half ->
      // ~half the miss).
      aoi::ContentPart ap;
      ap.type = "audio";
      const uint32_t sampleCount = 800;
      const uint32_t dataSize = sampleCount * 2;
      std::string wav(44 + dataSize, '\0');
      wav[0] = 'R'; wav[1] = 'I'; wav[2] = 'F'; wav[3] = 'F';
      wav[4] = static_cast<char>(36 + dataSize);
      wav[5] = static_cast<char>((36 + dataSize) >> 8);
      wav[6] = static_cast<char>((36 + dataSize) >> 16);
      wav[7] = static_cast<char>((36 + dataSize) >> 24);
      wav[8] = 'W'; wav[9] = 'A'; wav[10] = 'V'; wav[11] = 'E';
      wav[12] = 'f'; wav[13] = 'm'; wav[14] = 't'; wav[15] = ' ';
      wav[16] = 16; wav[17] = 0; wav[18] = 0; wav[19] = 0;  // fmt chunk size 16
      wav[20] = 1; wav[21] = 0;                            // PCM
      wav[22] = 1; wav[23] = 0;                            // mono
      wav[24] = 0x80; wav[25] = 0x3E; wav[26] = 0; wav[27] = 0;  // 16000
      wav[28] = 0x00; wav[29] = 0x7D; wav[30] = 0; wav[31] = 0;  // 32000
      wav[32] = 2; wav[33] = 0;                            // block align
      wav[34] = 16; wav[35] = 0;                           // bits
      wav[36] = 'd'; wav[37] = 'a'; wav[38] = 't'; wav[39] = 'a';
      wav[40] = static_cast<char>(dataSize);
      wav[41] = static_cast<char>(dataSize >> 8);
      wav[42] = static_cast<char>(dataSize >> 16);
      wav[43] = static_cast<char>(dataSize >> 24);
      // samples are all zero (silence)
      std::string b64;
      for (size_t j = 0; j < wav.size(); j += 3) {
        const unsigned b0 = static_cast<unsigned char>(wav[j]);
        const unsigned b1 = j + 1 < wav.size() ? static_cast<unsigned char>(wav[j + 1]) : 0;
        const unsigned b2 = j + 2 < wav.size() ? static_cast<unsigned char>(wav[j + 2]) : 0;
        static const char* kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        b64 += kA[b0 >> 2];
        b64 += kA[((b0 & 3) << 4) | (b1 >> 4)];
        b64 += j + 1 < wav.size() ? kA[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        b64 += j + 2 < wav.size() ? kA[b2 & 63] : '=';
      }
      ap.dataUrl = "data:audio/wav;base64," + b64;
      parts.push_back(std::move(ap));
    }
    std::cout << "=== turn " << i << " (chars=" << text.size()
              << ", ~" << text.size() / 2 << " tokens"
              << (withAudio ? ", audio" : "") << ") ===" << std::endl;
    session.prompt(text, parts);
    std::cout << "[TTFT] " << *ttftSlot << " ms" << std::endl;
  }
  std::cout << "DONE" << std::endl;
  return 0;
}
