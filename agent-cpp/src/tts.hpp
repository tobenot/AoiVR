#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "http_client.hpp"

namespace aoi {

struct TtsConfig {
  std::string apiKey;
  std::string model = "mimo-v2.5-tts";
  std::string voice = "冰糖";
  // Optional English voice (MiMo presets: Mia/Chloe/Milo/Dean). When set,
  // utterances that look predominantly English are spoken with this voice
  // instead of `voice` (e.g. a translation read-aloud demo). Empty keeps the
  // upstream behavior: every utterance uses `voice`.
  std::string englishVoice;
  // PCM sample rate the TTS endpoint emits (MiMo TTS outputs 24k PCM16).
  int sampleRate = 24000;
  // Leave empty to use the (encrypted) default base URL applied in MiMoTTS ctor.
  std::string baseUrl;
};

struct TtsChunk {
  std::string base64;
  int index = 0;
};

// MiMo TTS client (mirrors tts.ts). Streams PCM16 chunks from an SSE
// /chat/completions response and decodes the base64 audio payloads.
class MiMoTTS {
 public:
  explicit MiMoTTS(TtsConfig config);

  // Speak `text`; each received PCM16 chunk is passed to onChunk (base64).
  // Returns false on HTTP/network error (unless aborted).
  bool speak(const std::string& text, const std::string& style,
             const std::function<void(const TtsChunk&)>& onChunk);

  void abort() { aborted_ = true; }
  int sampleRate() const { return config_.sampleRate; }

 private:
  TtsConfig config_;
  std::atomic<bool> aborted_{false};
  HttpClient http_;
  int chunkIndex_ = 0;
};

} // namespace aoi
