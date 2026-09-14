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
  const bool withAudio8k = argc > 2 && std::string(argv[2]) == "--audio8k";
  const bool withMp3 = argc > 2 && std::string(argv[2]) == "--mp3";

  aoi::LlmSession::Config cfg;
  cfg.baseUrl = llm.value("baseUrl", "");
  cfg.apiKey = llm.value("apiKey", "");
  cfg.modelId = llm.value("model", "");
  cfg.thinking = "disabled";
  cfg.reasoningEffort = "";
  // Long stable system prompt (~1500 tokens: ~6000 CJK chars at ~4-5
  // chars/token) so the request prefix exceeds the provider's minimum
  // cacheable unit (~1024) - otherwise short voice turns never build a cache
  // and the audio-folding effect is invisible.
  cfg.systemPrompt =
      "你是 Aoi，一个住在 VR 头显里的中文语音助手。你通过语音与用户对话，"
      "帮助用户在 VR 环境中完成各种任务：查询信息、控制设备、提供建议。"
      "你的回答应该简洁、自然、口语化，适合语音播报。当用户说话时，"
      "认真倾听音频内容并理解其意图；如果听不清，礼貌地请用户重复。"
      "你能调用工具：读取文件、执行命令、查询数据库、访问网络。"
      "在 VR 环境中，你还需要帮助用户了解周围的环境信息、管理日程、"
      "播放内容、调节音量。请始终保持友好、耐心的态度，用最少的词"
      "传达最多的信息，避免冗长的解释。记住：语音交互的用户体验取决于"
      "响应速度和简洁度，所以每一句话都要精炼、直接、有用。当用户"
      "提问时先理解核心意图再回答，不确定时可以追问，但不要过度提问。"
      "你有完整的中文知识库，可以回答关于科技、文化、生活、学习等"
      "各方面的问题。";
  while (cfg.systemPrompt.size() < 6000) {
    cfg.systemPrompt +=
        "你擅长中文交流，理解力强，能根据上下文准确判断用户的意图，"
        "并提供高质量的回答。你的知识覆盖广泛，包括但不限于科学技术、"
        "文化艺术、日常生活、学习教育、娱乐休闲等领域。";
  }

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
    // Real voice scenario: the user input is AUDIO ONLY (no long text).
    // Long-text mode (no audio) is for cache baseline comparisons.
    const std::string text = (withAudio || withMp3) ? "" : longText(i);
    std::vector<aoi::ContentPart> parts;
    // Reset the TTFT slot for this turn.
    *ttftSlot = -1;
    *t0slot = std::chrono::steady_clock::now();
    if (withMp3) {
      // speech_long.mp3: real TTS speech (17s), mp3 format.
      aoi::ContentPart ap;
      ap.type = "audio";
      std::ifstream mf("speech_long.mp3", std::ios::binary);
      if (mf) {
        std::string data((std::istreambuf_iterator<char>(mf)),
                         std::istreambuf_iterator<char>());
        static const char* kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t j = 0; j < data.size(); j += 3) {
          const unsigned b0 = static_cast<unsigned char>(data[j]);
          const unsigned b1 = j + 1 < data.size() ? static_cast<unsigned char>(data[j + 1]) : 0;
          const unsigned b2 = j + 2 < data.size() ? static_cast<unsigned char>(data[j + 2]) : 0;
          b64 += kA[b0 >> 2];
          b64 += kA[((b0 & 3) << 4) | (b1 >> 4)];
          b64 += j + 1 < data.size() ? kA[((b1 & 15) << 2) | (b2 >> 6)] : '=';
          b64 += j + 2 < data.size() ? kA[b2 & 63] : '=';
        }
        ap.dataUrl = "data:audio/mpeg;base64," + b64;
        parts.push_back(std::move(ap));
      }
    } else if (withAudio8k) {
      // speech_long_8k.wav: 17s real speech downsampled to 8k (compressed).
      aoi::ContentPart ap;
      ap.type = "audio";
      std::ifstream mf("speech_long_8k.wav", std::ios::binary);
      if (mf) {
        std::string data((std::istreambuf_iterator<char>(mf)),
                         std::istreambuf_iterator<char>());
        static const char* kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t j = 0; j < data.size(); j += 3) {
          const unsigned b0 = static_cast<unsigned char>(data[j]);
          const unsigned b1 = j + 1 < data.size() ? static_cast<unsigned char>(data[j + 1]) : 0;
          const unsigned b2 = j + 2 < data.size() ? static_cast<unsigned char>(data[j + 2]) : 0;
          b64 += kA[b0 >> 2];
          b64 += kA[((b0 & 3) << 4) | (b1 >> 4)];
          b64 += j + 1 < data.size() ? kA[((b1 & 15) << 2) | (b2 >> 6)] : '=';
          b64 += j + 2 < data.size() ? kA[b2 & 63] : '=';
        }
        ap.dataUrl = "data:audio/wav;base64," + b64;
        parts.push_back(std::move(ap));
      }
    } else if (withAudio) {
      // speech_long_16k.wav: 17s real speech, uncompressed 16k wav.
      aoi::ContentPart ap;
      ap.type = "audio";
      std::ifstream mf("speech_long_16k.wav", std::ios::binary);
      if (mf) {
        std::string data((std::istreambuf_iterator<char>(mf)),
                         std::istreambuf_iterator<char>());
        static const char* kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t j = 0; j < data.size(); j += 3) {
          const unsigned b0 = static_cast<unsigned char>(data[j]);
          const unsigned b1 = j + 1 < data.size() ? static_cast<unsigned char>(data[j + 1]) : 0;
          const unsigned b2 = j + 2 < data.size() ? static_cast<unsigned char>(data[j + 2]) : 0;
          b64 += kA[b0 >> 2];
          b64 += kA[((b0 & 3) << 4) | (b1 >> 4)];
          b64 += j + 1 < data.size() ? kA[((b1 & 15) << 2) | (b2 >> 6)] : '=';
          b64 += j + 2 < data.size() ? kA[b2 & 63] : '=';
        }
        ap.dataUrl = "data:audio/wav;base64," + b64;
        parts.push_back(std::move(ap));
      }
    }
    std::cout << "=== turn " << i << (withMp3 ? " mp3"
                    : (withAudio8k ? " audio8k" : (withAudio ? " audio16k" : " text")))
              << " ===" << std::endl;
    session.prompt(text, parts);
    std::cout << "[TTFT] " << *ttftSlot << " ms" << std::endl;
  }
  std::cout << "DONE" << std::endl;
  return 0;
}
