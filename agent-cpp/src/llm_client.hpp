#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http_client.hpp"
#include "image_utils.hpp"

namespace aoi {

// Hard safety net on retained messages (the real limiter is the character
// threshold below). Not a memory-window knob anymore: history grows until it
// hits kHistoryCompressThreshold, then gets summarized.
inline constexpr int kMaxHistoryMessages = 500;
// Auto-compress threshold: when the provider-reported prompt_tokens of the
// last request exceeds this, the old history is replaced by an LLM summary.
// Sized for the special 200k-token window with margin for output + new turns.
// Driven by REAL provider usage (images and audio already counted); no local
// estimation.
inline constexpr size_t kHistoryCompressThreshold = 150000;

// A tool that the model can call. Mirrors the pi-coding-agent ToolDefinition +
// execute() surface used by agent.ts.
struct ToolDefinition {
  std::string name;
  std::string label;
  std::string description;
  nlohmann::json parameters;  // JSON Schema object

  // Execute the tool with parsed args. Returns a result payload (the raw
  // content the model will see next turn).
  std::function<nlohmann::json(const std::string& toolCallId, const nlohmann::json& args)>
      execute;
};

// Content block for a user prompt (text, or image/audio as base64 data).
struct ContentPart {
  std::string type;      // "text", "image_url", "audio"
  std::string text;
  std::string dataUrl;   // for image_url/audio
};

struct ChatMessage {
  std::string role;  // "system", "user", "assistant", "tool"
  std::string content;
  std::vector<ContentPart> parts;   // when multimodal
  std::vector<nlohmann::json> toolCalls;  // for assistant messages
  std::string toolCallId;                 // for tool messages
  // Reasoning (thinking) content from MiMo thinking mode. Captured from
  // delta.reasoning_content and round-tripped back to the provider on the
  // assistant history message — REQUIRED by MiMo when thinking is enabled and
  // the turn involves tool calls (else HTTP 400 on the next request).
  std::string reasoningContent;
};

// Events mirroring the pi-coding-agent session events used by agent.ts.
struct SessionEvent {
  std::string type;  // "message_update", "message_end", "tool_execution_start",
                     // "tool_execution_end", "agent_end", "reasoning_update"
  // For message_update / reasoning_update:
  std::string delta;
  // For message_end:
  std::string finalText;
  // For tool_execution_start/end:
  std::string toolName;
  nlohmann::json toolArgs;
  std::string result;
};

// A single stateless LLM session. Mirrors the subset of AgentSession used by
// agent.ts: prompt() with optional images/audio, subscribe() for events, and
// tool-calling loop (tools + customTools). Talks to an OpenAI-compatible
// /chat/completions SSE endpoint.
class LlmSession {
 public:
  struct Config {
    std::string providerId;
    std::string modelId;
    std::string baseUrl;
    std::string api;  // "openai-completions"
    std::string apiKey;
    std::string systemPrompt;
    std::vector<ToolDefinition> tools;
    // "enabled" | "disabled" | "auto" — sent as {"thinking":{"type": ...}}.
    // Default disabled (no thinking) for low latency; override per config.
    std::string thinking = "disabled";
    // Optional reasoning effort ("low" | "medium" | "high", MiMo accepts all
    // three alongside thinking.type). Empty = not sent (provider default).
    std::string reasoningEffort;
    // When false (default), incoming audio parts are transcribed locally
    // (sherpa-onnx) and replaced by their transcript before the request is
    // built — every OpenAI-compatible endpoint works. When true, audio is
    // sent natively as input_audio blocks; if the endpoint rejects them with
    // a 400 mentioning input_audio, the next turn auto-falls back to local
    // transcription for the rest of the session.
    bool nativeAudio = false;
  };

  explicit LlmSession(Config config);
  ~LlmSession();

  LlmSession(const LlmSession&) = delete;
  LlmSession& operator=(const LlmSession&) = delete;

  using EventCallback = std::function<void(const SessionEvent&)>;
  void subscribe(EventCallback cb);

  // Send a prompt (with optional multimodal parts) and run the tool loop to
  // completion. Events are delivered synchronously via subscribe callbacks.
  // Conversation history is retained across calls (multi-turn memory).
  void prompt(const std::string& text, const std::vector<ContentPart>& parts = {});

  // When the accumulated history exceeds kHistoryCompressThreshold, summarize
  // the old history with a one-shot LLM call and replace it with the summary
  // (fallback: keep only the newest messages). Called at the start of prompt().
  void maybeCompressHistory();
// Trim history to at most `max` messages at user-turn boundaries (shared by
// the hard cap and the compress-failure fallback).
void trimHistoryToMax(size_t max);
  // Fold old audio-bearing user messages to their text part so the history
  // prefix stays stable and cacheable (short audio content is not covered by
  // the gateway's prompt-prefix cache). The current turn (last user message)
  // is always kept verbatim; images are NOT folded (verified live that long
  // image_url content hits the cache).
  std::vector<ChatMessage> foldMultimodalHistory(const std::vector<ChatMessage>& history) const;

  void dispose();

  // Register an external cancel source (e.g. the agent's running_ flag). The
  // in-flight HTTP transfer is aborted when either this flag is set or the
  // session is disposed. Lets sub-sessions (translators, frame describers)
  // abort promptly on agent stop.
  void setCancelSource(std::function<bool()> cancel);

  // Optional log sink so internal diagnostics (HTTP errors, truncation) reach
  // the host's log (Unity Player.log) instead of being lost to stderr.
  using LogSink = std::function<void(const std::string&)>;
  void setLogSink(LogSink sink);
  void log(const std::string& line);

 private:
  nlohmann::json buildRequest(const std::vector<ChatMessage>& history);
  // One LLM call; streams text deltas via events and returns any tool calls.
  // Sets *outTruncated=true when the stream ended with finish_reason="length"
  // (provider-side output budget hit) so the caller can continue/retry instead
  // of presenting a half reply as if it were complete.
  bool runTurn(const std::vector<ChatMessage>& history,
               std::vector<nlohmann::json>& outToolCalls,
               std::string& outText,
               bool* outTruncated = nullptr,
               std::string* outReasoning = nullptr);
  void emit(const SessionEvent& e);
  bool isCancelled() const;
  // Stable sticky-routing session id (x-opencode-session): generated once per
  // process so all requests in this run share one upstream cache node.
  static const std::string& sessionId();

  Config config_;
  std::vector<ChatMessage> history_;  // multi-turn conversation history
  std::vector<EventCallback> listeners_;
  HttpClient http_;
  std::atomic<bool> disposed_{false};
  std::function<bool()> cancelSource_;
  LogSink logSink_;
  ContentPart toolImagePart_;   // screenshot image to persist after its tool msg
  std::string toolImageText_;   // caption for that persisted image user message
  ContentPart toolAudioPart_;   // audio clip (30s env sound) to persist
  std::string toolAudioText_;   // caption for the persisted audio user message

  // Usage from the LAST successful response (provider truth, incl. audio and
  // image tokens). Drives auto-compaction: prompt_tokens is the full context
  // size the provider actually processed.
  size_t lastPromptTokens_ = 0;
  size_t lastAudioTokens_ = 0;
  size_t lastCachedTokens_ = 0;
  size_t lastReasoningTokens_ = 0;
  bool sawUsage_ = false;

  // Human-readable cause of the last failed request (HTTP status + provider
  // error message, or transport error). Set by runTurn, shown to the user in
  // the failure finalText so the panel never just says "network error".
  std::string lastErrorDetail_;

  // Sticky fallback: set when a 400 error body mentions input_audio (endpoint
  // does not support native audio). From then on buildRequest transcribes all
  // audio parts locally instead of sending them on the wire.
  std::atomic<bool> nativeAudioRejected_{false};
};

} // namespace aoi
