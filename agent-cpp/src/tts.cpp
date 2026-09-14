#include "tts.hpp"

#include <nlohmann/json.hpp>

#include "base64.hpp"

namespace aoi {

namespace {
const std::string kDefaultBaseUrl = "https://api.xiaomimimo.com/v1";

// Rough "predominantly English" check used to pick the English TTS voice.
// The utterance counts as English when ASCII letters clearly outnumber CJK
// characters (same rough CJK lead-byte test as the interpreter's language
// check). Texts with no ASCII letters (pure Chinese, punctuation-only) fall
// through to the default voice.
bool looksEnglish(const std::string& s) {
  size_t asciiLetters = 0;
  size_t cjk = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      ++asciiLetters;
    } else if (c >= 0xE4 && c <= 0xE9) {
      ++cjk;  // rough CJK lead byte (3-byte UTF-8)
    }
  }
  return asciiLetters > 0 && asciiLetters > cjk * 2;
}
} // namespace

MiMoTTS::MiMoTTS(TtsConfig config) : config_(std::move(config)) {
  // Apply the (encrypted) default base URL when none was provided.
  if (config_.baseUrl.empty()) {
    config_.baseUrl = kDefaultBaseUrl;
  }
}

bool MiMoTTS::speak(const std::string& text, const std::string& style,
                    const std::function<void(const TtsChunk&)>& onChunk) {
  chunkIndex_ = 0;
  aborted_ = false;

  nlohmann::json messages = nlohmann::json::array();
  if (!style.empty()) {
    messages.push_back({{"role", "user"}, {"content", style}});
  }
  messages.push_back({{"role", "assistant"}, {"content", text}});

  nlohmann::json body;
  body["model"] = config_.model;
  body["messages"] = messages;
  // Pick the voice for this utterance: when an English voice is configured
  // and the text is predominantly English, use it (e.g. a translation
  // read-aloud demo); otherwise fall back to the default voice. Empty
  // englishVoice keeps the upstream single-voice behavior.
  std::string voice = config_.voice;
  if (!config_.englishVoice.empty() && looksEnglish(text)) {
    voice = config_.englishVoice;
  }
  body["audio"] = {{"format", "pcm16"}, {"voice", voice}};
  body["stream"] = true;

  const std::string url = config_.baseUrl + "/chat/completions";
  const std::vector<std::string> headers = {
      "Content-Type: application/json",
      "Authorization: Bearer " + config_.apiKey,
  };

  bool httpOk = true;
  // A caller-triggered abort is NOT a success: report it as a failure so the
  // speaker queue doesn't treat a partial audio stream as the full reply.
  if (aborted_.load()) return false;
  std::string buffer;  // SSE line buffer, scoped to this request
  // Cancel check so abort() interrupts the in-flight curl call promptly
  // (prevents detached-style hangs on shutdown).
  const auto res = http_.postStream(url, headers, body.dump(),
      [&](const char* data, size_t len) {
    if (aborted_.load()) return;
    // SSE parsing: accumulate lines, emit data: JSON chunks.
    buffer.append(data, len);
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      // Handle \r\n
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (line == "data: [DONE]") continue;
      const std::string prefix = "data: ";
      if (line.rfind(prefix, 0) != 0) continue;
      try {
        const auto json = nlohmann::json::parse(line.substr(prefix.size()));
        const auto& choices = json["choices"];
        if (choices.is_array() && !choices.empty() && choices[0].is_object()) {
          const auto& first = choices[0];
          if (first.contains("delta") && first["delta"].is_object()) {
            const auto& delta = first["delta"];
            if (delta.contains("audio")) {
              const auto& audio = delta["audio"];
              if (audio.is_object() && audio.contains("data")) {
                TtsChunk chunk;
                chunk.base64 = audio["data"].get<std::string>();
                chunk.index = chunkIndex_++;
                if (onChunk) onChunk(chunk);
              }
            }
          }
        }
      } catch (...) {
        // skip malformed SSE lines
      }
    }
  }, [this]() { return aborted_.load(); });

  if (aborted_.load()) {
    httpOk = false;  // aborted mid-stream: partial audio must not pass as OK
  } else if (res.status >= 400 || res.status <= 0) {
    httpOk = false;
  }
  return httpOk;
}

} // namespace aoi
