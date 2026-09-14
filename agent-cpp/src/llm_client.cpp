#include "llm_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "remote_asr.hpp"

namespace aoi {

namespace {

// Pull a human-readable error message out of a provider error body. Tries, in
// order: {"error":{"message":...}} (OpenAI style), {"error": "<str>"},
// {"message": ...}. Returns "" when nothing usable is found. The result is
// capped so a giant HTML error page can't flood the wrist panel.
std::string extractProviderErrorMessage(const std::string& body) {
  std::string msg;
  try {
    auto json = nlohmann::json::parse(body);
    if (json.is_object()) {
      if (json.contains("error")) {
        const auto& err = json["error"];
        if (err.is_object() && err.contains("message") && err["message"].is_string())
          msg = err["message"].get<std::string>();
        else if (err.is_string())
          msg = err.get<std::string>();
      }
      if (msg.empty() && json.contains("message") && json["message"].is_string())
        msg = json["message"].get<std::string>();
    }
  } catch (...) {
    // Not JSON (HTML error page / plain text): fall through to raw snippet.
  }
  if (msg.empty()) {
    // Raw fallback: first line of the body.
    const size_t nl = body.find('\n');
    msg = body.substr(0, nl == std::string::npos ? body.size() : nl);
  }
  constexpr size_t kMax = 300;
  if (msg.size() > kMax) {
    msg.resize(kMax);
    msg += "…";
  }
  return msg;
}

// Parse Retry-After from response headers (opencode executor.ts:93-106).
// Supports "retry-after-ms" (ms), "Retry-After" integer seconds, and
// "Retry-After" as an HTTP date. Returns milliseconds, or -1 when absent.
int retryAfterMs(const std::vector<std::pair<std::string, std::string>>& headers) {
  for (auto rit = headers.rbegin(); rit != headers.rend(); ++rit) {
    if (rit->first == "retry-after-ms") {
      char* end = nullptr;
      const long ms = std::strtol(rit->second.c_str(), &end, 10);
      if (end != rit->second.c_str() && *end == '\0' && ms >= 0)
        return static_cast<int>((std::min)(ms, 3600000L));
      break;
    }
  }
  for (auto rit = headers.rbegin(); rit != headers.rend(); ++rit) {
    if (rit->first == "retry-after") {
      char* end = nullptr;
      const long secs = std::strtol(rit->second.c_str(), &end, 10);
      // Clamp before the *1000: a huge Retry-After would overflow the int
      // cast (implementation-defined, possibly negative) and defeat the
      // caller's minimum-delay semantics.
      if (end != rit->second.c_str() && *end == '\0' && secs >= 0)
        return static_cast<int>((std::min)(secs, 3600L) * 1000);
      const time_t when = curl_getdate(rit->second.c_str(), nullptr);
      if (when != -1) {
        const long diffSecs = static_cast<long>(when - time(nullptr));
        return diffSecs > 0 ? static_cast<int>((std::min)(diffSecs, 3600L) * 1000) : 0;
      }
      break;
    }
  }
  return -1;
}

// Stream-layer retryability. Retry on 429/5xx, or transport failures:
// classified by the libcurl CURLcode (text matching against libcurl's
// strerror is unreliable and misses most real-world failure strings), with a
// lowercase-text match kept as a fallback for non-curl error text.
bool streamRetryable(const HttpClient::Result& res) {
  if (res.status == 429 || (res.status >= 500 && res.status <= 599)) return true;
  switch (res.curlCode) {
    case CURLE_GOT_NOTHING:      // response headers, then connection closed
    case CURLE_PARTIAL_FILE:     // body cut short mid-transfer
    case CURLE_RECV_ERROR:       // receive error / connection reset
    case CURLE_SEND_ERROR:       // send error
    case CURLE_COULDNT_CONNECT:  // connect refused/unreachable
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PEER_FAILED_VERIFICATION:
      return true;
    default:
      break;
  }
  if (res.status <= 0) {
    std::string e = res.error;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* pats[] = {"timeout", "timed out", "connection refused",
                                 "lost", "reset", "etimedout", "fetch failed"};
    for (const char* p : pats)
      if (e.find(p) != std::string::npos) return true;
  }
  return false;
}

// Sleep in small slices so cancellation (agent stop) is honored promptly.
void sleepInterruptible(int ms, const std::function<bool()>& cancelled) {
  const int step = 100;
  int waited = 0;
  while (waited < ms) {
    if (cancelled()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds((std::min)(step, ms - waited)));
    waited += step;
  }
}
} // namespace

// ---- LlmSession ----

LlmSession::LlmSession(Config config) : config_(std::move(config)) {}

LlmSession::~LlmSession() { dispose(); }

void LlmSession::subscribe(EventCallback cb) {
  listeners_.push_back(std::move(cb));
}

void LlmSession::emit(const SessionEvent& e) {
  for (const auto& cb : listeners_) cb(e);
}

void LlmSession::dispose() { disposed_ = true; }

void LlmSession::setCancelSource(std::function<bool()> cancel) {
  cancelSource_ = std::move(cancel);
}

void LlmSession::setLogSink(LlmSession::LogSink sink) {
  logSink_ = std::move(sink);
}

void LlmSession::log(const std::string& line) {
  if (logSink_) logSink_(line);
  else std::fprintf(stderr, "%s\n", line.c_str());
}

bool LlmSession::usesRemoteAsr() const {
  return !config_.nativeAudio || nativeAudioRejected_.load();
}

bool LlmSession::transcribeRemoteAudioParts(std::vector<ContentPart>& parts) {
  bool hasAudio = false;
  for (const auto& part : parts) {
    if (part.type == "audio") {
      hasAudio = true;
      break;
    }
  }
  if (!hasAudio) return true;

  if (config_.asr.apiKey.empty()) {
    lastErrorDetail_ = "缺少远程 ASR API Key，请在 aoi_config.json 的 asr.apiKey 中填写密钥";
    log("[ASR] " + lastErrorDetail_);
    return false;
  }
  if (config_.asr.baseUrl.empty() || config_.asr.model.empty()) {
    lastErrorDetail_ = "远程 ASR 配置不完整（需要 asr.baseUrl 和 asr.model）";
    log("[ASR] " + lastErrorDetail_);
    return false;
  }

  std::string baseUrl = config_.asr.baseUrl;
  while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
  const std::string url = baseUrl + "/chat/completions";
  const std::vector<std::string> headers = {
      "Content-Type: application/json",
      "Accept: application/json",
      "Authorization: Bearer " + config_.asr.apiKey,
  };
  static const std::string kPrompt =
      "请准确转写这段语音，只输出听到的原文，不要解释、翻译或加引号。"
      "保留中文、英文、数字和必要的标点。没有清晰语音时输出空文本。";

  for (auto& part : parts) {
    if (part.type != "audio") continue;

    RemoteAsrAudio audio;
    if (!parseRemoteAsrAudioDataUrl(part.dataUrl, audio)) {
      lastErrorDetail_ = "音频不是受支持的 data:audio/wav 或 data:audio/mpeg 格式";
      log("[ASR] " + lastErrorDetail_);
      return false;
    }

    const auto request = buildRemoteAsrRequest(config_.asr.model, kPrompt, audio);
    const auto response = http_.postStream(
        url, headers, request.dump(), nullptr,
        [this]() { return isCancelled(); });
    if (response.status >= 400 || response.status <= 0) {
      std::string detail;
      if (!response.body.empty()) detail = extractProviderErrorMessage(response.body);
      if (detail.empty() && !response.transportError.empty()) {
        detail = response.transportError;
        if (response.osError > 0) detail += " (os errno " + std::to_string(response.osError) + ")";
      }
      if (detail.empty() && isCancelled()) detail = "request cancelled";
      lastErrorDetail_ = detail.empty()
          ? ("ASR HTTP status " + std::to_string(response.status))
          : ("ASR HTTP " + std::to_string(response.status) + ": " + detail);
      log("[ASR] HTTP error status=" + std::to_string(response.status) +
          " url=" + url);
      log("[ASR] error detail: " + lastErrorDetail_);
      return false;
    }

    const auto parsed = parseRemoteAsrResponse(response.body);
    if (!parsed.ok) {
      lastErrorDetail_ = "ASR 返回了无法解析的转写结果";
      log("[ASR] " + lastErrorDetail_);
      return false;
    }

    ContentPart transcript;
    transcript.type = "text";
    transcript.text = parsed.text.empty()
        ? "（语音中没有检测到清晰语音）"
        : ("用户说了（远程语音转写）：" + parsed.text);
    part = std::move(transcript);
  }
  return true;
}

bool LlmSession::transcribeRemoteAudioHistory(std::vector<ChatMessage>& history) {
  if (!usesRemoteAsr()) return true;
  for (auto& message : history) {
    if (message.parts.empty()) continue;
    if (!transcribeRemoteAudioParts(message.parts)) return false;
  }
  return true;
}

bool LlmSession::isCancelled() const {
  if (disposed_.load()) return true;
  return cancelSource_ && cancelSource_();
}

const std::string& LlmSession::sessionId() {
  static const std::string id = [] {
    // 32 hex chars, generated once per process.
    std::mt19937_64 rng(static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&rng));
    const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 32; ++i) s += hex[(rng() >> (i % 8) * 4) & 0xF];
    return s;
  }();
  return id;
}

nlohmann::json LlmSession::buildRequest(const std::vector<ChatMessage>& history) {
  nlohmann::json req;
  req["model"] = config_.modelId;
  req["stream"] = true;
  // Output budget, matching opencode's openai-chat body exactly
  // (ProviderTransform.maxOutputTokens defaults to 32000): large enough that
  // the always-on reasoning never eats the whole reply.
  req["max_tokens"] = 32000;
  // Ask the server to include usage stats in the final stream chunk (some
  // gateways only emit usage with this flag).
  req["stream_options"] = {{"include_usage", true}};
  // Session-scoped cache with long retention. The opencode zen/go gateway's
  // automatic prefix cache only lasts ~5 minutes by default; with screenshots
  // now persisting in history (they DO hit the cache, verified live), a pause
  // longer than the TTL would force a full prefill of the whole image-heavy
  // history. prompt_cache_key + 24h retention keeps the cache alive across
  // pauses and app restarts (the key is stable, not per-turn).
  req["prompt_cache_key"] = "aoi-vr-agent-cpp";
  req["prompt_cache_retention"] = "24h";
  // No max_tokens / max_completion_tokens: like Codex CLI (codex-rs
  // ResponsesApiRequest has no output-token limit field), the provider's own
  // default output budget applies. Setting one here would let the always-on
  // reasoning eat the entire budget (empty replies) or truncate answers.
  // MiMo thinking mode (OpenAI-compatible wire format, see mimo.mi.com docs):
  //   {"thinking": {"type": "enabled" | "disabled" | "auto"}}
  if (!config_.thinking.empty()) {
    req["thinking"] = {{"type", config_.thinking}};
  }
  // Optional reasoning effort (MiMo accepts low/medium/high alongside
  // thinking.type, "deepseek"-style format).
  if (!config_.reasoningEffort.empty()) {
    req["reasoning_effort"] = config_.reasoningEffort;
  }

  nlohmann::json messages = nlohmann::json::array();
  messages.push_back({{"role", "system"}, {"content", config_.systemPrompt}});
  // Multimodal user messages (audio/image) are not covered by the gateway's
  // prompt-prefix cache (verified live: input_audio/image_url messages never
  // count toward cached_tokens), so a history full of audio+text turns keeps
  // missing the cache after the system prompt. Fold all but the newest
  // kKeepRecentMultimodal multimodal user messages down to their text part
  // (stable placeholder text when there is none): the folded history is
  // byte-identical across turns, so the prefix stays cacheable and TTFT
  // stops degrading as the conversation grows.
  const std::vector<ChatMessage> effectiveHistory = foldMultimodalHistory(history);
  for (const auto& m : effectiveHistory) {
    nlohmann::json jm;
    jm["role"] = m.role;
    if (m.role == "tool") {
      jm["content"] = m.content;
      jm["tool_call_id"] = m.toolCallId;
    } else if (!m.parts.empty()) {
      nlohmann::json content = nlohmann::json::array();
      for (const auto& p : m.parts) {
        if (p.type == "text") {
          content.push_back({{"type", "text"}, {"text", p.text}});
        } else if (p.type == "image_url") {
          content.push_back({{"type", "image_url"},
                             {"image_url", {{"url", p.dataUrl}}}});
        } else if (p.type == "audio") {
          // MiMo audio input wire format:
          //   {"type":"input_audio","input_audio":{"data":"<base64>","format":"wav"}}
          // data is the RAW base64 payload (NO "data:audio/wav;base64," prefix),
          // and there IS a `format` field. The piped-through dataUrl carries the
          // prefix, so strip it here. Upstream accepts format in
          // {mp3, flac, wav, ogg} (verified live: wav/mp3/flac HTTP 200).
          // M4A/AAC data is rejected ("Param Incorrect"), so never emit it.
          std::string audioData = p.dataUrl;
          // Detect the media subtype from the data: URL prefix only
          // ("data:audio/flac;base64," etc). Never scan the base64 payload
          // itself (it could coincidentally contain "mp3" as text and
          // mis-route the format).
          std::string format = "wav";
          const std::string prefix = "data:audio/";
          if (audioData.rfind(prefix, 0) == 0) {
            const size_t semi = audioData.find(';', prefix.size());
            const std::string subtype = audioData.substr(
                prefix.size(), semi == std::string::npos ? std::string::npos : semi - prefix.size());
            if (subtype.find("flac") != std::string::npos) {
              format = "flac";
            } else if (subtype.find("mpeg") != std::string::npos ||
                       subtype.find("mp3") != std::string::npos) {
              format = "mp3";
            }
            const size_t comma = audioData.find(",", prefix.size());
            if (comma != std::string::npos) audioData = audioData.substr(comma + 1);
          }
          content.push_back({{"type", "input_audio"},
                             {"input_audio", {{"data", audioData},
                                              {"format", format}}}});
        }
      }
      jm["content"] = content;
      if (!m.toolCalls.empty()) jm["tool_calls"] = m.toolCalls;
    } else {
      jm["content"] = m.content;
      if (!m.toolCalls.empty()) jm["tool_calls"] = m.toolCalls;
      // Round-trip the reasoning text back on the assistant message. Write
      // both spellings so every endpoint accepts it:
      //  - "reasoning"         opencode gateway (its stream emits delta.reasoning)
      //  - "reasoning_content" MiMo API direct (thinking mode REQUIRES it in
      //                         multi-turn tool-call loops, else HTTP 400
      //                         "The reasoning_content in the thinking mode
      //                         must be passed back").
      // The gateway tolerates the extra field (verified: no 400).
      if (!m.reasoningContent.empty()) jm["reasoning"] = m.reasoningContent;
      if (!m.reasoningContent.empty()) jm["reasoning_content"] = m.reasoningContent;
    }
    messages.push_back(std::move(jm));
  }
  req["messages"] = messages;

  // DIAG (keep): per-message digest of the request so we can compare message
  // sequences across turns and find where the prompt-prefix cache chain
  // breaks (e.g. screenshot tool rounds) and where time is spent.
  {
    auto fnv = [](const std::string& s) -> uint64_t {
      uint64_t h = 14695981039346656037ull;
      for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
      return h;
    };
    std::string d = "[LLM][DIAG] sys=" + std::to_string(config_.systemPrompt.size()) +
                    ":" + std::to_string(fnv(config_.systemPrompt));
    for (const auto& m : effectiveHistory) {
      d += " [" + m.role;
      if (!m.content.empty())
        d += " c=" + std::to_string(m.content.size()) + ":" + std::to_string(fnv(m.content));
      if (!m.parts.empty()) {
        d += " parts";
        for (const auto& p : m.parts) {
          d += " " + p.type + "=";
          if (p.type == "text")
            d += std::to_string(p.text.size()) + ":" + std::to_string(fnv(p.text));
          else
            d += std::to_string(p.dataUrl.size()) + ":" + std::to_string(fnv(p.dataUrl));
        }
      }
      if (!m.toolCalls.empty()) {
        d += " tcs";
        for (const auto& tc : m.toolCalls) {
          const std::string s = tc.dump();
          d += " " + s.substr(0, s.size() < 120 ? s.size() : 120);
        }
      }
      if (!m.toolCallId.empty()) d += " tid=" + m.toolCallId;
      d += "]";
    }
    log(d);
  }

  if (!config_.tools.empty()) {
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& t : config_.tools) {
      nlohmann::json jt;
      jt["type"] = "function";
      jt["function"]["name"] = t.name;
      jt["function"]["description"] = t.description;
      if (t.parameters.is_object()) jt["function"]["parameters"] = t.parameters;
      tools.push_back(std::move(jt));
    }
    req["tools"] = tools;
  }
  return req;
}

// Parse SSE chunks from /chat/completions; returns tool calls and accumulated text.
bool LlmSession::runTurn(std::vector<ChatMessage>& history,
                         std::vector<nlohmann::json>& outToolCalls,
                         std::string& outText,
                         bool* outTruncated,
                         std::string* outReasoning) {
  if (outTruncated) *outTruncated = false;
  // Tool results may append an audio-bearing user message after prompt() has
  // already prepared the original microphone clip. Convert those messages
  // here as well so the text-only LLM path can never leak input_audio.
  if (usesRemoteAsr() && !transcribeRemoteAudioHistory(history)) return false;
  const auto req = buildRequest(history);
  const std::string url = config_.baseUrl + "/chat/completions";
  // On opencode endpoints (opencode.ai zen / zen/go) send the same request
  // profile as the opencode CLI (request.ts): sticky-routing headers +
  // User-Agent. The gateway pins requests with the same x-opencode-session to
  // the same upstream node, which is what makes its prompt-prefix cache hit;
  // without it, requests drift across nodes and every turn is a cold prefill.
  // Non-opencode endpoints (e.g. MiMo direct) get a plain request.
  std::vector<std::string> headers = {
      "Content-Type: application/json",
      "Authorization: Bearer " + config_.apiKey,
  };
  if (config_.baseUrl.find("opencode.ai") != std::string::npos) {
    headers.push_back("x-opencode-session: " + sessionId());
    headers.push_back("x-opencode-project: aoi-vr-agent");
    headers.push_back("x-opencode-client: aoi-vr-cpp");
    headers.push_back("x-opencode-request: aoi-vr-user");
    headers.push_back("User-Agent: opencode/1.18.15");
  }

  std::string finishReason;
  struct ToolSlot { std::string id; std::string name; std::string args; };
  std::vector<ToolSlot> toolAccum;  // partial tool calls accumulated by index
  bool sawToolCall = false;

  // Streaming request with retry aligned to opencode's HTTP SSE path:
  // - executor layer (executor.ts:91,345-364): HTTP 429/5xx retried up to 2x
  //   with 500ms*2^n*[0.8,1.2] capped at 10s; Retry-After honored (capped 10s).
  // - stream layer (processor.ts, covers the whole stream incl. mid-stream
  //   breaks): retryable errors (429/5xx after the executor layer, connection
  //   class failures) retried with Retry-After (uncapped) or 2s*2^(n-1) capped
  //   30s, bounded by a 5-minute total retry-wait budget.
  // Everything else (non-429 4xx, malformed arguments) fails immediately.
  const int kExecutorMaxRetries = 2;
  const int64_t kRetryBudgetMs = 300000;  // 5 min total retry wait
  int executorAttempts = 0;
  int streamAttempts = 0;
  int64_t retryWaitedMs = 0;
  // Whether ANY attempt of this call already streamed text deltas to the host.
  // A retry regenerates the reply from scratch, so the consumer's accumulated
  // text must be reset or it would end up with the failed attempt's prefix
  // duplicated ("AB" + retry "ABCD" -> "ABABCD" on the panel and in TTS).
  bool emittedTextEver = false;

  for (int attempt = 0; ; ++attempt) {
    // Per-attempt state reset.
    toolAccum.clear();
    sawToolCall = false;
    outToolCalls.clear();
    outText.clear();
    finishReason.clear();
    if (outReasoning) outReasoning->clear();

    std::string buffer;  // SSE line buffer, scoped to this request
    std::string rawBody;  // full raw response, captured for error diagnostics
    bool sawTerminal = false;  // saw [DONE] or a non-empty finish_reason
    const auto res = http_.postStream(url, headers, req.dump(), [&](const char* data, size_t len) {
    rawBody.append(data, len);
    buffer.append(data, len);    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      // Accept both "data: [DONE]" and "data:[DONE]" spellings.
      if (line.rfind("data:", 0) == 0) {
        std::string payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
        if (payload == "[DONE]") {
          finishReason = "stop";
          sawTerminal = true;
          continue;
        }
      }
      const std::string prefix = "data: ";
      if (line.rfind(prefix, 0) != 0) continue;
      try {
        auto json = nlohmann::json::parse(line.substr(prefix.size()));
        if (!json.is_object()) continue;
        // Final streaming chunk carries the provider usage stats (prompt
        // tokens include audio+image tokens; details break them out). Parsed
        // BEFORE the choices check: some providers send a usage-only chunk
        // (empty choices array) as the last frame.
        if (json.contains("usage") && json["usage"].is_object()) {
          const auto& u = json["usage"];
          lastPromptTokens_ = u.value("prompt_tokens", 0);
          lastCachedTokens_ = u.value("cached_tokens", 0);
          if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
            lastAudioTokens_ = u["prompt_tokens_details"].value("audio_tokens", 0);
            if (u["prompt_tokens_details"].contains("cached_tokens"))
              lastCachedTokens_ = u["prompt_tokens_details"].value("cached_tokens", lastCachedTokens_);
          }
          if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
            lastReasoningTokens_ = u["completion_tokens_details"].value("reasoning_tokens", 0);
          }
          sawUsage_ = true;
        }
        if (!json.contains("choices")) continue;
        const auto& choices = json["choices"];
        if (!choices.is_array() || choices.empty()) continue;
        const auto& first = choices[0];
        if (!first.is_object()) continue;
        const bool hasDelta = first.contains("delta") && first["delta"].is_object();
        // finish_reason lives INSIDE choices[0] for OpenAI-compatible streams;
        // the top-level json["finish_reason"] is almost never set.
        if (first.contains("finish_reason") && first["finish_reason"].is_string()) {
          finishReason = first["finish_reason"].get<std::string>();
          sawTerminal = true;
        }
        if (hasDelta) {
          const auto& delta = first["delta"];
          if (delta.contains("content") && delta["content"].is_string()) {
            const std::string d = delta["content"].get<std::string>();
            outText += d;
            if (d.size()) {
              emittedTextEver = true;
              SessionEvent ev;
              ev.type = "message_update";
              ev.delta = d;
              emit(ev);
            }
          }
          // Thinking-mode reasoning stream: capture for round-trip on the
          // assistant history message AND stream to the host so the UI can
          // show the live reasoning in the processing bar. The field name
          // differs per endpoint: the opencode gateway emits "reasoning",
          // the MiMo API direct emits "reasoning_content".
          std::string reasoningDelta;
          if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
            reasoningDelta = delta["reasoning"].get<std::string>();
          } else if (delta.contains("reasoning_content") &&
                     delta["reasoning_content"].is_string()) {
            reasoningDelta = delta["reasoning_content"].get<std::string>();
          }
          if (outReasoning && !reasoningDelta.empty()) {
            *outReasoning += reasoningDelta;
            SessionEvent ev;
            ev.type = "reasoning_update";
            ev.delta = reasoningDelta;
            emit(ev);
          }
          if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tc : delta["tool_calls"]) {
              if (!tc.is_object()) continue;
              // Accumulate by index so multiple parallel tool calls all work.
              const int idx = tc.value("index", 0);
              if (toolAccum.size() <= static_cast<size_t>(idx))
                toolAccum.resize(static_cast<size_t>(idx) + 1);
              auto& slot = toolAccum[idx];
              // IMPORTANT: JSON null must NOT be treated as a string. The
              // gateway emits `"id": null` and `"function": {"name": null,
              // "arguments": ...}` on argument-delta chunks; nlohmann's
              // value(key, default) throws type_error when the stored value
              // is null, and the outer catch would swallow the WHOLE chunk
              // line -- dropping the arguments too (the announce-only bug).
              std::string id, name, args;
              if (tc.contains("id") && tc["id"].is_string())
                id = tc["id"].get<std::string>();
              if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                  name = fn["name"].get<std::string>();
                if (fn.contains("arguments") && fn["arguments"].is_string())
                  args = fn["arguments"].get<std::string>();
              }
              if (!id.empty()) slot.id = id;
              if (!name.empty()) slot.name += name;
              if (!args.empty()) slot.args += args;
              sawToolCall = true;
            }
          }
        }
      } catch (...) {
        // skip malformed SSE lines
      }
    }
  }, [this]() { return isCancelled(); });

    // ---- Retry decision (aligned with opencode) ----
    if (res.status == -1) {
      log("[LLM] stream cancelled; aborting turn");
      return false;
    }
    bool shouldRetry = false;
    int delayMs = 0;
    const char* retryLayer = nullptr;
    // A clean HTTP close WITHOUT the [DONE] sentinel or any finish_reason is
    // only a real truncation when NOTHING was produced: several stream
    // endpoints end without the sentinel (or carry finish_reason only as
    // null), so retrying any no-sentinel response would loop forever - each
    // retry regenerates the entire reply (minutes with long reasoning).
    // Retry only when the stream completed with zero content AND zero tool
    // calls AND no termination marker (an abnormal empty response).
    if (res.status > 0 && res.curlCode == CURLE_OK && !sawTerminal &&
        outText.empty() && !sawToolCall) {
      log("[LLM] stream ended empty without [DONE]/finish_reason; retrying");
      delayMs = 2000 * (1 << streamAttempts);
      delayMs = (std::min)(delayMs, 30000);
      ++streamAttempts;
      retryLayer = "stream";
      shouldRetry = true;
    } else if (res.status > 0 && res.curlCode != CURLE_OK) {
      // Mid-stream break: HTTP 200 headers received but the connection died
      // before the body completed (curl reports a non-OK code). The partial
      // response must NEVER be presented as the final answer - retry it like a
      // transport failure.
      log("[LLM] mid-stream break: status=" + std::to_string(res.status) +
          " curl=" + std::to_string(res.curlCode) + " error=" + res.error);
      if (streamRetryable(res)) {
        delayMs = retryAfterMs(res.headers);
        if (delayMs < 0) delayMs = 2000 * (1 << streamAttempts);
        delayMs = (std::min)(delayMs, 30000);
        ++streamAttempts;
        retryLayer = "stream";
        shouldRetry = true;
      } else {
        log("[LLM] mid-stream break (not retryable); aborting turn");
        return false;
      }
    } else if (res.status >= 400) {
      const bool retryableStatus = res.status == 429 || (res.status >= 500 && res.status <= 599);
      if (retryableStatus && executorAttempts < kExecutorMaxRetries) {
        delayMs = retryAfterMs(res.headers);
        if (delayMs < 0) delayMs = static_cast<int>(500.0 * (1 << executorAttempts) *
                                                    (0.8 + (rand() % 5) * 0.1));
        delayMs = (std::min)(delayMs, 10000);
        ++executorAttempts;
        retryLayer = "executor";
        shouldRetry = true;
      } else if (streamRetryable(res)) {
        delayMs = retryAfterMs(res.headers);
        if (delayMs < 0) delayMs = 2000 * (1 << streamAttempts);
        delayMs = (std::min)(delayMs, 30000);
        ++streamAttempts;
        retryLayer = "stream";
        shouldRetry = true;
      } else {
        log("[LLM] HTTP error status=" + std::to_string(res.status) + " url=" + url);
        if (!rawBody.empty()) {
          const size_t n = rawBody.size() < 600 ? rawBody.size() : 600;
          log("[LLM] response head: " + rawBody.substr(0, n));
        }
        return false;
      }
    } else if (res.status <= 0) {
      // Transport failure: stream-layer retry only for retryable patterns.
      if (streamRetryable(res)) {
        delayMs = retryAfterMs(res.headers);
        if (delayMs < 0) delayMs = 2000 * (1 << streamAttempts);
        delayMs = (std::min)(delayMs, 30000);
        ++streamAttempts;
        retryLayer = "stream";
        shouldRetry = true;
      } else {
        log("[LLM] HTTP error status=" + std::to_string(res.status) + " url=" + url +
            " error=" + res.error);
        return false;
      }
    }

    if (shouldRetry) {
      if (emittedTextEver) {
        // Tell the host to discard the partial text streamed by the failed
        // attempt; the next attempt starts from a clean slate.
        SessionEvent ev;
        ev.type = "message_reset";
        emit(ev);
        emittedTextEver = false;
      }
      // Floor the delay: a 0/negative Retry-After would spin a zero-delay
      // hot loop that never consumes the retry budget.
      if (delayMs < 500) delayMs = 500;
      if (retryWaitedMs + delayMs > kRetryBudgetMs) {
        log("[LLM] retry wait budget exhausted (" + std::to_string(kRetryBudgetMs) +
            "ms); aborting turn");
        return false;
      }
      log(std::string("[LLM] ") + retryLayer + "-layer retry in " +
          std::to_string(delayMs) + "ms (attempt " + std::to_string(attempt + 1) +
          ", waited " + std::to_string(retryWaitedMs) + "ms)");
      sleepInterruptible(delayMs, [this]() { return isCancelled(); });
      if (isCancelled()) return false;
      retryWaitedMs += delayMs;
      continue;
    }
    // Surface the real cause to the UI: prefer the provider's JSON error
    // message ("error.message" / "message"), then the transport detail, then
    // just the status code. Kept SHORT — this rides along in finalText and
    // must stay readable on the wrist panel.
    std::string detail;
    if (!rawBody.empty()) detail = extractProviderErrorMessage(rawBody);
    if (detail.empty() && !res.transportError.empty()) {
      detail = res.transportError;
      if (res.osError > 0) detail += " (os errno " + std::to_string(res.osError) + ")";
    }
    lastErrorDetail_ = detail.empty()
        ? ("HTTP status " + std::to_string(res.status))
        : ("HTTP " + std::to_string(res.status) + ": " + detail);
    log("[LLM] error detail: " + lastErrorDetail_);
    // Auto-fallback: endpoint rejected native audio. Flip the sticky flag so
    // later turns transcribe through the configured remote ASR instead of
    // sending input_audio blocks to this text-only endpoint.
    if (res.status == 400 &&
        (rawBody.find("input_audio") != std::string::npos ||
         detail.find("input_audio") != std::string::npos)) {
      nativeAudioRejected_.store(true);
      log("[LLM] endpoint does not support input_audio; falling back to remote ASR");
    }
    return false;
  }

  if (sawUsage_) {
    log("[LLM] usage prompt=" + std::to_string(lastPromptTokens_) +
        " cached=" + std::to_string(lastCachedTokens_) +
        " audio=" + std::to_string(lastAudioTokens_) +
        " reasoning=" + std::to_string(lastReasoningTokens_));
  }

  if (outTruncated && finishReason == "length") {
    *outTruncated = true;
  }

  // Settlement aligned with opencode (tool-stream.ts finishAll + shared.ts:155):
  // EMPTY arguments are normalized to "{}" and go to the tool loop (the tool
  // layer reports the missing parameter back to the model); MALFORMED JSON
  // fails the turn immediately (eventError equivalent -- never retried, never
  // dispatched to a tool).
  if (sawToolCall) {
    for (auto& slot : toolAccum) {
      nlohmann::json call;
      call["id"] = slot.id;
      call["type"] = "function";
      call["function"]["name"] = slot.name;
      // OpenAI protocol requires arguments to be a JSON-ENCODED STRING (the raw
      // accumulated stream text). Validated below, so it is safe to
      // round-trip verbatim on the next turn.
      std::string args = slot.args;
      if (args.empty()) {
        args = "{}";
      } else if (nlohmann::json::parse(args, nullptr, false).is_discarded()) {
        log("[LLM] tool call '" + slot.name + "' has malformed JSON arguments; aborting turn");
        return false;
      }
      if (args.size() > 300) {
        log("[ToolStream] call name=" + slot.name + " rawArgs(len=" +
            std::to_string(args.size()) + ") head=\"" + args.substr(0, 300) + "\"");
      } else {
        log("[ToolStream] call name=" + slot.name + " rawArgs=\"" + args + "\"");
      }
      call["function"]["arguments"] = args;
      outToolCalls.push_back(std::move(call));
    }
  }
  return true;
}

void LlmSession::setHistoryFile(const std::string& path) {
  historyFile_ = path;
  if (!path.empty()) loadHistoryFrom(path);
}

void LlmSession::setSystemPrompt(const std::string& p) {
  config_.systemPrompt = p;
}

void LlmSession::loadHistoryFrom(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return;
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  auto j = nlohmann::json::parse(content, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("messages") ||
      !j["messages"].is_array()) {
    // Corrupt history must not be silently discarded - the conversation
    // memory would vanish without a trace. Surface it in the logs.
    log("[LLM] WARNING: history file " + path + " is corrupted or unreadable; "
        "starting with an empty history");
    return;
  }
  history_.clear();
  for (const auto& mj : j["messages"]) {
    if (!mj.is_object()) continue;
    ChatMessage m;
    m.role = mj.value("role", "");
    m.content = mj.value("content", "");
    m.toolCallId = mj.value("tool_call_id", "");
    m.reasoningContent = mj.value("reasoning_content", "");
    if (mj.contains("tool_calls") && mj["tool_calls"].is_array())
      m.toolCalls = mj["tool_calls"].get<std::vector<nlohmann::json>>();
    if (mj.contains("parts") && mj["parts"].is_array()) {
      for (const auto& p : mj["parts"]) {
        if (p.is_object() && p.value("type", "") == "text") {
          ContentPart cp;
          cp.type = "text";
          cp.text = p.value("text", "");
          m.parts.push_back(std::move(cp));
        }
      }
    }
    history_.push_back(std::move(m));
  }
  if (!history_.empty())
    log("[LLM] history loaded from " + path + " (" +
        std::to_string(history_.size()) + " messages)");
}

void LlmSession::persistHistory() {
  if (historyFile_.empty()) return;
  nlohmann::json j;
  j["version"] = 1;
  j["messages"] = nlohmann::json::array();
  for (const auto& m : history_) {
    nlohmann::json mj;
    mj["role"] = m.role;
    mj["content"] = m.content;
    if (!m.toolCallId.empty()) mj["tool_call_id"] = m.toolCallId;
    if (!m.reasoningContent.empty()) mj["reasoning_content"] = m.reasoningContent;
    if (!m.toolCalls.empty()) mj["tool_calls"] = m.toolCalls;
    // Only text parts are persisted (audio/image are per-session context).
    nlohmann::json parts = nlohmann::json::array();
    for (const auto& p : m.parts) {
      if (p.type == "text") parts.push_back({{"type", "text"}, {"text", p.text}});
    }
    if (!parts.empty()) mj["parts"] = parts;
    j["messages"].push_back(std::move(mj));
  }
  // Atomic write: dump to a temp file then rename over the target, so a crash
  // mid-write can never leave a truncated history.json (which the loader
  // silently discards, losing the whole conversation memory).
  const std::string tmp = historyFile_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << j.dump();
  }
  std::error_code ec;
  std::filesystem::rename(tmp, historyFile_, ec);
  if (ec) std::filesystem::remove(tmp, ec);  // rename failed (e.g. locked)
}

void LlmSession::maybeCompressHistory() {
  if (history_.empty()) return;
  // Compression is driven by the provider's REAL prompt_tokens from the last
  // successful request (images and audio already included). No local
  // estimation: if we have no usage record yet, the history is empty/trivial
  // and there is nothing to compress.
  if (lastPromptTokens_ <= kHistoryCompressThreshold) return;

  // Serialize the old history to text for the summarizer (multimodal parts
  // cannot be summarized; their text captions are kept if any).
  std::string transcript;
  for (const auto& m : history_) {
    if (!m.content.empty()) {
      const char* who = m.role == "assistant" ? "助手"
                        : m.role == "tool"     ? "工具结果"
                                               : "用户";
      transcript += std::string(who) + ": " + m.content + "\n";
    }
    // Multimodal messages carry their text in parts (content is empty):
    // without them the captions (screenshot descriptions, voice transcript)
    // never reach the summary.
    for (const auto& p : m.parts) {
      if (p.type == "text" && !p.text.empty()) {
        const char* who = m.role == "assistant" ? "助手"
                          : m.role == "tool"     ? "工具结果"
                                                 : "用户";
        transcript += std::string(who) + ": " + p.text + "\n";
      }
    }
  }

  std::string summary;
  bool ok = false;
  if (!transcript.empty()) {
    try {
      // One-shot summarizer session (own history, never recurses into
      // compression itself).
      LlmSession::Config sc = config_;
      sc.systemPrompt =
          "You compress a chat history into a concise summary. Keep key facts, "
          "user preferences, ongoing task state and any unresolved questions. "
          "Output ONLY the summary, in the same language as the chat.";
      sc.tools.clear();
      LlmSession summarizer(sc);
      // CRITICAL: wire the SAME cancel source and a log sink. The summarizer
      // runs a full HTTP request BEFORE the main prompt; without a cancel
      // source the host's "interrupt everything" cannot reach it (observed
      // live: a stuck summarizer wedged the message thread for minutes, the
      // follow-up turn's interrupt had no effect, and nothing was logged
      // because there was no log sink).
      summarizer.setCancelSource([this]() { return isCancelled(); });
      summarizer.setLogSink(
          [this](const std::string& line) { log(std::string("[LLM][compress] ") + line); });
      std::string got;
      summarizer.subscribe([&](const SessionEvent& e) {
        if (e.type == "message_update") got += e.delta;
        if (e.type == "message_reset") got.clear();
        if (e.type == "message_end") got = e.finalText.empty() ? got : e.finalText;
      });
      summarizer.prompt("请将以下对话历史压缩为简洁摘要（保留关键事实、用户偏好、任务状态、未完成事项），只输出摘要：\n\n" +
                        transcript);
      summary = got;
      ok = !summary.empty();
    } catch (...) {
      ok = false;
    }
  }

  if (ok) {
    ChatMessage sumMsg;
    sumMsg.role = "system";
    sumMsg.content = "以下是更早对话的摘要（原历史已压缩丢弃）：\n" + summary;
    history_.clear();
    history_.push_back(std::move(sumMsg));
    log("[LLM] history compressed: old history replaced by summary");
    // Persist the compressed history so the disk file doesn't keep growing
    // with the pre-compression messages (restart would otherwise resurrect
    // them and the compression would be wasted).
    persistHistory();
  } else {
    // Summarizer failed (network / too long): degrade by keeping only the
    // newest messages so the request still fits the window. Trim at complete
    // user-turn boundaries so we never leave an orphaned tool message whose
    // assistant(tool_calls) was cut (some servers 400 on that).
    trimHistoryToMax(6);
    log("[LLM] history compress failed; trimmed to newest messages");
  }
}

// Trim history down to at most `max` messages, cutting at user-message
// boundaries so an assistant(tool_calls) is never dropped while its tool
// response stays behind. The cut lands on the FIRST user message at/after
// `excess` - keeping at most `max` messages after it.
void LlmSession::trimHistoryToMax(size_t max) {
  if (history_.size() <= max) return;
  const size_t excess = history_.size() - max;
  size_t cut = history_.size();
  for (size_t i = excess; i < history_.size(); ++i) {
    if (history_[i].role == "user") { cut = i; break; }
  }
  if (cut == history_.size()) {
    // No user message in the kept region: fall back to cutting after the last
    // user message before it, or plain trimming when there are no user
    // messages at all (pure system/tool tail).
    for (size_t i = history_.size(); i-- > 0;) {
      if (history_[i].role == "user") { cut = i + 1; break; }
    }
    if (cut == history_.size()) cut = excess;
  }
  if (cut > 0) {
    history_.erase(history_.begin(), history_.begin() + cut);
  } else {
    history_.erase(history_.begin(), history_.begin() + excess);
  }
}

namespace {

bool hasMultimodalParts(const ChatMessage& m) {
  if (m.role != "user") return false;
  for (const auto& p : m.parts) {
    if (p.type == "audio" || p.type == "image_url") return true;
  }
  return false;
}

bool hasAudioParts(const ChatMessage& m) {
  if (m.role != "user") return false;
  for (const auto& p : m.parts) {
    if (p.type == "audio") return true;
  }
  return false;
}

}  // namespace

std::vector<ChatMessage> LlmSession::foldMultimodalHistory(
    const std::vector<ChatMessage>& history) const {
  // Product default: keep historical audio verbatim (no folding). Verified in
  // the cache lab: no-fold + mp3 gives 87-93% prompt-cache hits with stable
  // TTFT (3.1-5.6s) over 10 turns, while folding breaks the cache chain every
  // turn because the folded placeholder never matches the raw base64 of the
  // earlier turn (folded runs: cached stuck at 1024 or ~81% hit rate with
  // growing TTFT). Audio is mp3 now (~79KB/17s), so keeping history audio is
  // cheap. Images stay in history either way (verified cacheable).
  // Legacy switch: AOI_FOLD=1 restores the old fold-to-text behavior.
  if (!std::getenv("AOI_FOLD")) return history;
  // Locate the current-turn user message: the LAST user message in history.
  size_t lastUserIdx = history.size();
  for (size_t i = history.size(); i-- > 0;) {
    if (history[i].role == "user") { lastUserIdx = i; break; }
  }

  std::vector<ChatMessage> out;
  out.reserve(history.size());
  for (size_t idx = 0; idx < history.size(); ++idx) {
    const auto& m = history[idx];
    const bool isCurrentTurn = (idx == lastUserIdx);
    // The current turn's audio must reach the model verbatim.
    if (!isCurrentTurn && hasAudioParts(m)) {
      // Fold: drop ONLY the audio part (speech never carried in history
      // again). Text parts AND image parts are kept — a dual-grip message
      // (audio + screenshot) must keep its image, otherwise the screenshot
      // disappears from history and the cache chain breaks.
      ChatMessage f;
      f.role = m.role;
      for (const auto& p : m.parts) {
        if (p.type == "audio") continue;
        f.parts.push_back(p);
      }
      if (f.parts.empty()) {
        ContentPart tp;
        tp.type = "text";
        tp.text = "（此前有一条语音消息，内容已由对应的助手回复处理）";
        f.parts.push_back(std::move(tp));
      }
      out.push_back(std::move(f));
      continue;
    }
    out.push_back(m);
  }
  return out;
}

void LlmSession::prompt(const std::string& text, const std::vector<ContentPart>& parts) {
  if (disposed_.load()) return;
  lastErrorDetail_.clear();
  maybeCompressHistory();
  // Fast-fail when no API key is configured: without this, the request hangs
  // on the provider (Bearer <empty> is not rejected immediately) and the UI
  // stays stuck on "正在发送..." with no error, until the 120s low-speed
  // timeout. Tell the user exactly what's missing instead.
  if (config_.apiKey.empty()) {
    std::fprintf(stderr, "[LLM] No API key configured for provider '%s'; aborting prompt\n",
                 config_.providerId.c_str());
    SessionEvent ev;
    ev.type = "message_end";
    ev.finalText = "缺少 API Key，无法连接大模型。请在 aoi_config.json 的 "
                   "llm.apiKey 中填写密钥后重启。";
    emit(ev);
    SessionEvent end;
    end.type = "agent_end";
    emit(end);
    return;
  }
  // Split incoming parts into audio and images. In text-only mode, perform the
  // remote ASR stage before the user message enters history; this guarantees
  // that the downstream LLM request contains text/image blocks only.
  std::vector<ContentPart> audioParts;
  std::vector<ContentPart> imageParts;
  std::string transcriptPrefix;
  std::vector<ContentPart> preparedParts = parts;
  if (usesRemoteAsr()) {
    if (!transcribeRemoteAudioParts(preparedParts)) {
      SessionEvent ev;
      ev.type = "message_end";
      ev.finalText = "（远程 ASR 失败：" + lastErrorDetail_ + "）";
      emit(ev);
      SessionEvent end;
      end.type = "agent_end";
      emit(end);
      return;
    }
    for (const auto& p : preparedParts) {
      if (p.type == "image_url") imageParts.push_back(p);
      else if (!p.text.empty()) transcriptPrefix += p.text + "\n";
    }
  } else {
    for (const auto& p : preparedParts) {
      if (p.type == "audio") audioParts.push_back(p);
      else if (p.type == "image_url") imageParts.push_back(p);
      else if (!p.text.empty()) transcriptPrefix += p.text + "\n";
    }
  }

  // Persistent user message: text + audio + images (all co-located so the
  // model treats them as one user turn). In remote-ASR mode, the transcript
  // rides at the FRONT of the message text so the model reads
  // "用户说了（远程语音转写）：…\n<original instruction text>".
  const std::string effectiveText = transcriptPrefix.empty()
      ? text : (transcriptPrefix + text);
  ChatMessage user;
  user.role = "user";
  if (imageParts.empty() && audioParts.empty()) {
    user.content = effectiveText;
  } else {
    ContentPart tp;
    tp.type = "text";
    tp.text = effectiveText;
    user.parts.push_back(tp);
    for (auto& ip : imageParts) user.parts.push_back(ip);
    for (auto& ap : audioParts) user.parts.push_back(ap);
  }
  history_.push_back(user);
  persistHistory();
  // Cap history: keep the most recent messages (system prompt is re-added by
  // buildRequest). Trim at complete-turn boundaries (cut at the first user
  // message that leaves at most kMaxHistoryMessages), so we never drop an
  // assistant(tool_calls) while keeping its orphaned tool response — some
  // servers 400 or ignore a tool message whose assistant call was trimmed.
  trimHistoryToMax(kMaxHistoryMessages);

  // Native audio persists in the user message itself. Remote-ASR messages are
  // already text-only; runTurn also guards tool-produced audio before wiring
  // the next request.
  int guard = 0;
  std::string textOut;
  std::string reasoningOut;
  for (;;) {
    // Safety bound checked at the TOP of every iteration — covers BOTH exit
    // paths (tool calls AND truncated-continuation "（继续）" loops). Without
    // this, a model that keeps hitting the provider output budget would
    // continue forever (guard only counted tool rounds before).
    if (++guard > 32) {
      SessionEvent ev;
      ev.type = "message_end";
      ev.finalText = textOut.empty() ? "（回复过长或工具调用过多，已停止）" : textOut;
      emit(ev);
      SessionEvent end;
      end.type = "agent_end";
      emit(end);
      break;
    }
    // Abort the whole tool loop promptly on cancel (agent stop / dispose).
    if (isCancelled()) {
      SessionEvent ev;
      ev.type = "message_end";
      ev.finalText = "";
      emit(ev);
      SessionEvent end;
      end.type = "agent_end";
      emit(end);
      return;
    }
    // History (with persisted audio/images) is used directly every iteration.
    std::vector<ChatMessage>* turnHistory = &history_;
    // Snapshot the history size BEFORE the turn so a failed request can roll
    // back the assistant(tool_calls) + tool messages it appended. Without the
    // rollback, a mid-turn failure (e.g. gateway 400/500) leaves a poisoned
    // assistant message with malformed tool_calls in history, and every
    // subsequent turn re-sends it and fails the same way.
    const size_t histBeforeTurn = history_.size();

    std::vector<nlohmann::json> toolCalls;
    textOut.clear();
    reasoningOut.clear();
    bool truncated = false;
    const bool ok = runTurn(*turnHistory, toolCalls, textOut, &truncated, &reasoningOut);
    if (!ok) {
      history_.resize(histBeforeTurn);
      // Deliver a final (error) message so the UI never stays stuck on
      // "正在发送...". If we got partial text, use it; otherwise show the real
      // cause (HTTP status + provider message / transport error) so the wrist
      // panel says WHY it failed instead of a generic network line.
      SessionEvent ev;
      ev.type = "message_end";
      ev.finalText = textOut.empty()
          ? ("（网络异常，未能获取回复。" + lastErrorDetail_ + "）")
          : textOut;
      emit(ev);
      SessionEvent end;
      end.type = "agent_end";
      emit(end);
      return;
    }

    if (toolCalls.empty()) {
      // Retain partial/final assistant text in history so the next turn has memory.
      if (!textOut.empty() || !reasoningOut.empty()) {
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = textOut;
        assistant.reasoningContent = reasoningOut;
        history_.push_back(std::move(assistant));
        persistHistory();
      }
      if (truncated) {
        // The provider hit its output budget mid-reply. Continue the loop so
        // it finishes the response instead of presenting a half answer as
        // complete. The `guard` bound (see below) caps runaway continuations.
        std::fprintf(stderr, "[LLM] finish_reason=length, continuing\n");
        ChatMessage contUser;
        contUser.role = "user";
        contUser.content = "（继续）";
        history_.push_back(std::move(contUser));
        continue;
      }
      SessionEvent ev;
      ev.type = "message_end";
      ev.finalText = textOut;
      emit(ev);
      SessionEvent end;
      end.type = "agent_end";
      emit(end);
      return;
    }

    // Execute tool calls, appending assistant + tool messages.
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = textOut;
    assistant.toolCalls = toolCalls;
    assistant.reasoningContent = reasoningOut;
    history_.push_back(assistant);
    persistHistory();

    for (const auto& call : toolCalls) {
      const std::string name = call["function"]["name"].get<std::string>();
      const std::string id = call.value("id", "");
      nlohmann::json args = nlohmann::json::object();
      if (call["function"].contains("arguments") &&
          call["function"]["arguments"].is_object()) {
        args = call["function"]["arguments"];
      } else if (call["function"].contains("arguments") &&
                 call["function"]["arguments"].is_string()) {
        try {
          args = nlohmann::json::parse(call["function"]["arguments"].get<std::string>());
        } catch (...) {
        }
      }

      // Per-tool-call diagnostic: name, raw argument string and parsed state.
      // Helps debug loops where the model keeps omitting required parameters.
      {
        std::string rawArgs;
        if (call["function"].contains("arguments") &&
            call["function"]["arguments"].is_string())
          rawArgs = call["function"]["arguments"].get<std::string>();
        if (rawArgs.size() > 200) rawArgs = rawArgs.substr(0, 200) + "...";
        const std::string parseState =
            args.is_object() ? (args.empty() ? "EMPTY-OBJECT" : "ok")
                             : (args.is_discarded() || args.is_null() ? "PARSE-FAIL" : "non-object");
        log("[ToolLoop] turn#" + std::to_string(guard) + " call name=" + name +
            " rawArgs=\"" + rawArgs + "\" parsed=" + parseState);
      }

      SessionEvent start;
      start.type = "tool_execution_start";
      start.toolName = name;
      start.toolArgs = args;
      emit(start);

      std::string resultText;
      bool found = false;
      for (const auto& t : config_.tools) {
        if (t.name == name) {
          found = true;
          try {
            const auto result = t.execute(id, args);
            // Convention: a tool may return {"content": "...", "__image":
            // "data:image/...;base64,..."} to attach an image. We inject the
            // image as a user message into history_ IMMEDIATELY so the model
            // sees it in this same turn's next runTurn (not the next prompt).
            if (result.is_object() && result.contains("__image") &&
                result["__image"].is_string()) {
              ContentPart img;
              img.type = "image_url";
              img.dataUrl = result["__image"].get<std::string>();
              const std::string imgText = result.value("content", "Screenshot captured.");
              // Persist the screenshot as a user message APPENDED AFTER the
              // tool message (legitimate message order: assistant(tool_calls)
              // -> tool -> user). Unlike a pre-tool insert this never violates
              // the protocol, and unlike the per-turn pendingImageParts_ the
              // image survives into future turns so a follow-up like "what was
              // in that screenshot?" still has the picture.
              resultText = imgText;
              // Mark this tool result as carrying an image to be persisted.
              toolImagePart_ = std::move(img);
              toolImageText_ = imgText;
            } else if (result.is_object() && result.contains("__audio") &&
                       result["__audio"].is_string()) {
              // Same convention for audio: a tool may return {"__audio":
              // "data:audio/wav;base64,..."} so the model can HEAR the clip.
              ContentPart aud;
              aud.type = "audio";
              aud.dataUrl = result["__audio"].get<std::string>();
              const std::string audText = result.value("content", "Audio clip.");
              resultText = audText;
              toolAudioPart_ = std::move(aud);
              toolAudioText_ = audText;
            } else if (result.is_object() && result.contains("content") &&
                       result["content"].is_string()) {
              // Align with pi: the tool message content should be the plain
              // text result, not a JSON dump of the whole payload.
              resultText = result["content"].get<std::string>();
            } else {
              resultText = result.dump();
            }
          } catch (const std::exception& ex) {
            resultText = std::string("Tool error: ") + ex.what();
          }
          break;
        }
      }
      if (!found) {
        resultText = "Tool not found: " + name;
      }

      SessionEvent done;
      done.type = "tool_execution_end";
      done.toolName = name;
      done.result = resultText;
      emit(done);

      // Tool result diagnostic (truncated).
      {
        std::string res = resultText;
        if (res.size() > 200) res = res.substr(0, 200) + "...";
        log("[ToolLoop] turn#" + std::to_string(guard) + " result name=" + name +
            " text=\"" + res + "\"");
      }

      ChatMessage toolMsg;
      toolMsg.role = "tool";
      toolMsg.content = resultText;
      toolMsg.toolCallId = id;
      history_.push_back(std::move(toolMsg));
      persistHistory();

      // Screenshot/audio attachments are NOT inserted inline: with parallel
      // tool calls the assistant(tool_calls) message pairs with ALL tool
      // messages of the batch, and a user message in between violates the
      // protocol (some providers 400 on "tool messages must immediately
      // follow"). Collect them and append after the whole batch below.
      if (!toolImagePart_.type.empty()) {
        ChatMessage imgMsg;
        imgMsg.role = "user";
        ContentPart tp;
        tp.type = "text";
        tp.text = toolImageText_;
        imgMsg.parts.push_back(std::move(tp));
        imgMsg.parts.push_back(std::move(toolImagePart_));
        mediaMessages_.push_back(std::move(imgMsg));
        toolImagePart_ = ContentPart();  // reset for next tool call
        toolImageText_.clear();
      }
      if (!toolAudioPart_.type.empty()) {
        ChatMessage audMsg;
        audMsg.role = "user";
        ContentPart tp;
        tp.type = "text";
        tp.text = toolAudioText_;
        audMsg.parts.push_back(std::move(tp));
        audMsg.parts.push_back(std::move(toolAudioPart_));
        mediaMessages_.push_back(std::move(audMsg));
        toolAudioPart_ = ContentPart();  // reset for next tool call
        toolAudioText_.clear();
      }
    }
    // Append collected image/audio attachments AFTER the complete tool batch
    // so assistant(tool_calls) -> tool x N -> user(media) ordering holds.
    for (auto& m : mediaMessages_) history_.push_back(std::move(m));
    mediaMessages_.clear();
    persistHistory();
  }
}

} // namespace aoi
