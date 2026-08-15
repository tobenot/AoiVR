#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent_config.hpp"
#include "environment_awareness.hpp"
#include "interpreter.hpp"
#include "llm_client.hpp"
#include "mic.hpp"
#include "player.hpp"
#include "tts.hpp"
#include "types.hpp"

namespace aoi {

// The main AI agent that relays between Unity (named pipe), the LLM, TTS,
// mic capture, simultaneous interpretation, and environment awareness. Port of
// agent.ts.
class AoiAgent {
 public:
  // Outbound message sink; called on the agent's background thread with each
  // JSON message the agent wants to send to Unity.
  using OutboundSink = std::function<void(const std::string& json)>;

  AoiAgent();
  ~AoiAgent();

  AoiAgent(const AoiAgent&) = delete;
  AoiAgent& operator=(const AoiAgent&) = delete;

  bool start();
  void stop();

  // In-process transport (replaces the named pipe):
  //  - setOutboundSink(sink) registers where outbound JSON goes.
  //  - sendJson(json) enqueues an inbound message from Unity (thread-safe).
  void setOutboundSink(OutboundSink sink);
  bool sendJson(const std::string& json);

  // Optional log sink so agent logs can reach Unity.
  using LogSink = std::function<void(const std::string&)>;
  void setLogSink(LogSink sink);
  void logLine(const std::string& line) const;
  void debug(const char* fmt, ...) const;

 private:
  // ---- message handling (in-process) ----
  void onConnected();
  void onMessage(const Message& msg);
  void handleStateChange(const Message& msg);
  void handleUserInput(const Message& msg);
  void send(MessageType type, nlohmann::json payload);
  void sendTuiFeed(const std::string& text);

  // ---- recording ----
  void startRecording();
  void stopAndProcess();

  // ---- LLM / session ----
  std::shared_ptr<LlmSession> createSessionWithPrompt(const std::string& systemPrompt);
  std::shared_ptr<LlmSession> createSessionWithPrompt(const std::string& systemPrompt,
                                                      const std::string& thinking,
                                                      const std::string& reasoningEffort);
  void handleSessionEvent(const SessionEvent& e, bool isMain);
  void handleTextDelta(const std::string& delta);
  void handleMessageEnd(const std::string& text);
  void sendFinalText(const std::string& text);
  void handleAgentEnd();

  // ---- tools ----
  ToolDefinition makeScreenshotTool();
  ToolDefinition makeVrSetBrightnessTool();
  ToolDefinition makeSystemTool();
  ToolDefinition makeInterpretationTool();
  ToolDefinition makeAwarenessTool();
  ToolDefinition makeContextTool();
  void processShot(const std::string& path, const std::string& mime,
                   const std::string& label, std::string& outData, std::string& outMime);
  std::string describeFrame(const std::string& path);
  std::string promptSubSession(const std::shared_ptr<LlmSession>& session,
                               const std::string& promptText,
                               const std::vector<ContentPart>& parts);

  // ---- interpretation ----
  bool startInterpretation(const std::string& targetLang);
  void stopInterpretation();
  void handleTranslationSegment(const SpeechSegment& seg);
  void deliverTranslations();
  bool isSameLanguageAsTarget(const std::string& lang, const std::string& text) const;
  std::string contextPrefix() const;

  // ---- TTS ----
  void enqueueTts(const std::string& text);
  void streamTts(const std::string& text, int gen);
  void stopTts();

  // ---- awareness ----
  void setAwareness(bool enabled);

  // ---- pending requests to Unity ----
  nlohmann::json requestFromUnity(const std::string& context, MessageType type,
                                  nlohmann::json payload);

  OutboundSink outboundSink_;
  std::mutex sinkMutex_;
  std::atomic<bool> inProcessConnected_{false};

  LogSink logSink_;
  mutable std::mutex logMutex_;

  std::unique_ptr<MiMoTTS> tts_;
  MicCapture mic_;
  LlmSession::Config sessionConfig_;
  std::shared_ptr<LlmSession> session_;

  std::string sentenceBuffer_;
  std::string fullResponse_;
  bool sawAnyTextDelta_ = false;
  bool finalSent_ = false;
  bool busy_ = false;
  // [PRONUNCIATION] block state (fork feature): while active, TTS output is
  // buffered instead of streamed, and spoken at the end (English sentence only).
  bool pronBlockActive_ = false;
  std::string pronBuffer_;
  void flushPronBlock();  // speak first field of each block line, reset state
  static std::string stripPronMarkers(const std::string& s);  // display filter
  std::deque<std::string> ttsQueue_;
  std::mutex ttsMutex_;  // guards ttsQueue_/ttsPlaying_/ttsStopRequested_
  std::string lastSpoken_;  // last TTS sentence spoken (dedup guard)
  std::atomic<int> ttsGeneration_{0};  // bumped on each enqueue/stop; workers check it
  bool ttsPlaying_ = false;
  bool ttsStopRequested_ = false;
  int64_t lastTextSendTime_ = 0;
  // Live model reasoning (thinking) streamed to the procbar ("thinking" stage).
  std::string thinkingText_;
  int64_t lastThinkSendTime_ = 0;
  bool tuiEnabled_ = false;
  bool tuiHeaderSent_ = false;

  std::unique_ptr<SpeechInterpreter> interpreter_;
  std::string targetLang_ = "中文";
  std::vector<std::string> interpretationHistory_;
  mutable std::mutex interpHistoryMutex_;  // guards interpretationHistory_
  // Sliding-window context: the previous window's source text + translation
  // (the O-overlap tail), fed as text prompt so the LLM repairs cross-window
  // boundaries without re-hearing the audio.
  std::string interpPrevSrc_;
  std::string interpPrevTrans_;
  // Concurrent translation buffering, delivered in seq order.
  std::map<int, std::shared_ptr<std::string>> pendingTranslations_;
  std::mutex pendingTranslationsMutex_;
  std::mutex deliveringMutex_;
  int nextSeqToSend_ = 1;
  bool delivering_ = false;
  bool interpActive_ = false;
  // Generation counter for interpretation sessions. Bumped on every start;
  // translation workers capture it and discard their result if the session
  // they belong to is no longer current (prevents stale cross-session writes).
  std::atomic<int> interpGeneration_{0};
  // Bounded in-flight translation workers. Guards against unbounded worker
  // spawns when a noisy segment stream feeds many slow LLM calls.
  int translationInFlight_ = 0;
  std::condition_variable translationSlotCv_;
  std::mutex translationSlotMutex_;

  // Background worker threads (TTS playback, translation). Tracked so stop()
  // can join them before `this` is destroyed — detached threads would run past
  // destruction (use-after-free).
  std::vector<std::thread> workers_;
  std::mutex workersMutex_;
  void spawnWorker(std::thread t);
  void joinWorkers();

  std::unique_ptr<EnvironmentAwareness> awareness_;

  std::string lastUtteranceTime_;
  std::string pendingShotPath_;
  int lastSegStartSample_ = -1;
  int lastSegEndSample_ = -1;

  // Pending request tracking (id -> result).
  struct PendingRequest {
    nlohmann::json* result = nullptr;
    std::shared_ptr<std::promise<nlohmann::json>> promise;
  };
  std::map<std::string, std::shared_ptr<std::promise<nlohmann::json>>> pendingRequests_;
  std::mutex pendingMutex_;

  // Incoming pipe messages are handled on a DEDICATED worker thread so the
  // pipe read thread never blocks on an LLM call. ScreenshotResponse is
  // resolved inline (fast, non-blocking) on the read thread.
  std::deque<Message> msgQueue_;
  std::mutex msgMutex_;
  std::condition_variable msgCv_;
  std::thread msgThread_;
  void msgLoop();

  std::atomic<bool> running_{false};
  AgentFileConfig fileConfig_;
  std::string workDir_;
  std::string apiKey_;

  // Tracks whether we've already sent "generate" this turn (avoid duplicates
  // across tool-call loops within the same user turn).
  std::atomic<bool> processingStageSentGenerate_{false};
};

} // namespace aoi
