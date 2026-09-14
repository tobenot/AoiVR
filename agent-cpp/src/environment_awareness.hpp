#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "speech_segmenter.hpp"

namespace aoi {

struct VisualEntry {
  int64_t time = 0;      // epoch ms
  std::string timeStr;
  std::string framePath;
  std::string desc;
  std::string diff;      // vs previous frame
  std::string kind;      // "working" | "summary"
};

struct AudioEntry {
  int64_t time = 0;
  std::string timeStr;
  std::string text;
  std::string kind;
};

struct AudioClipEntry {
  int64_t time = 0;
  std::string timeStr;
  std::string path;   // wav file (30s segment)
  int durationMs = 0;
  int seq = 0;
};

struct SummaryEntry {
  int64_t timeStart = 0;
  int64_t timeEnd = 0;
  std::string text;
};

struct EnvironmentAwarenessOptions {
  int windowMinutes = 5;
  int batchSeconds = 1;
  int frameWidth = 512;
  int concurrency = 4;
  std::string framesDir;                        // where Unity writes frame_*.png
  bool captureAudio = true;
  std::string audioDir;                         // where 30s wav segments are stored
  int audioSegmentSeconds = 30;                 // fixed audio segment length (0 = off)
  bool autoUnderstand = false;                  // auto-describe frames / transcribe audio
  std::function<std::string(const std::string&)> describeFrame;  // injectable vision describer
};

// Continuous environment awareness: watches frame_*.png files Unity writes into
// context/frames/, describes each new frame via a vision sub-session, captures
// system audio (VAD + sherpa) and keeps a rolling text timeline. Mirrors
// environment-awareness.ts.
class EnvironmentAwareness {
 public:
  explicit EnvironmentAwareness(EnvironmentAwarenessOptions opts);
  ~EnvironmentAwareness();

  EnvironmentAwareness(const EnvironmentAwareness&) = delete;
  EnvironmentAwareness& operator=(const EnvironmentAwareness&) = delete;

  // Public mirror of enabled_ (agent reads it from its message thread while
  // the host thread may call stop()) - must be atomic.
  std::atomic<bool> enabled{false};

  void start();
  void stop();

  // Newest frame file written by Unity (context/frames/frame_*.png), or empty.
  std::string getNewestFramePath() const;

  // Newest 30s audio clip (wav path), or empty.
  std::string getNewestClipPath() const;

  // List recent audio clips: "HH:MM:SS 30s <path>" lines, or empty.
  std::string listClips(int minutes = -1) const;

  void pauseAudio();
  void resumeAudio();
  void addAudio(const std::string& text);

  // Build a text summary of recent context for AI consumption.
  std::string getContext(int minutes = -1) const;

 private:
  void visualLoop();
  void cleanupLoop();
  void processVisualBatch();
  std::string computeDiff(const std::string& prev, const std::string& cur) const;
  void startAudio();
  void stopAudio();

  // Guards audioSegmenter_: startAudio/stopAudio/pauseAudio/resumeAudio run
  // on different threads (host thread during stop() vs the agent's message
  // thread for the pause/resume tools) - a reset racing a start is a UAF.
  std::mutex audioMutex_;
  std::unique_ptr<SpeechSegmenter> audioSegmenter_;

  EnvironmentAwarenessOptions opts_;
  std::vector<VisualEntry> visualEntries_;
  std::vector<AudioEntry> audioEntries_;
  std::vector<AudioClipEntry> audioClips_;
  std::vector<SummaryEntry> summaries_;
  std::string lastDesc_;
  std::string framesDir_;
  std::string audioDir_;

  std::thread visualThread_;
  std::thread cleanupThread_;
  std::atomic<bool> enabled_{false};
  std::atomic<int> inFlight_{0};  // active describeFrame calls

  // Interruptible wait so stop() can join the cleanup thread promptly instead
  // of blocking on a long sleep (was 30s).
  std::mutex stopMutex_;
  std::condition_variable stopCv_;

  mutable std::mutex mutex_;
};

} // namespace aoi
