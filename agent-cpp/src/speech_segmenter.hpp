#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_utils.hpp"
#include "speaker.hpp"

namespace aoi {

struct SpeechSegment {
  std::vector<uint8_t> wavBuffer;  // 44-byte header + PCM
  int seq = 0;
  int durationMs = 0;
  int startSample = 0;  // absolute sample position in the captured stream
  std::string text;     // sherpa-onnx ASR transcription if available
  std::string language; // inferred source language code, e.g. 'zh','en'
  bool sliding = false; // true for sliding-window segments (interpretation)
  int64_t windowStartMs = 0; // wall-clock start of this window
};

struct SpeechSegmenterOptions {
  std::function<void(const SpeechSegment&)> onSegment;
  std::function<void(const std::string& state, const std::string& msg)> onState;
  // When > 0, ignores VAD and cuts audio into fixed-length segments of this
  // many seconds (e.g. 30). Used by environment awareness so audio is stored
  // as raw wav segments WITHOUT transcription; the AI reads them on demand
  // (get_context returns them as input_audio). 0 = VAD-driven (interpretation).
  int fixedSegmentSeconds = 0;
  // Sliding-window mode (interpretation): windowSeconds is the block length W,
  // overlapSeconds the overlap O; every (W-O) seconds a window of the LATEST W
  // seconds is emitted (overlapping windows repair cross-boundary truncation).
  // 0 = disabled. Takes precedence over fixedSegmentSeconds.
  int windowSeconds = 0;
  int overlapSeconds = 1500;  // ms (used when windowSeconds > 0)
};

// Captures system speaker loopback, runs Silero VAD segmentation, and
// transcribes each detected segment with sherpa-onnx. Shared by simultaneous
// interpretation and environment-awareness audio recording. Mirrors
// speech-segmenter.ts. When built without sherpa-onnx, segments are still
// emitted but without transcription.
class SpeechSegmenter {
 public:
  explicit SpeechSegmenter(SpeechSegmenterOptions opts);
  ~SpeechSegmenter();

  SpeechSegmenter(const SpeechSegmenter&) = delete;
  SpeechSegmenter& operator=(const SpeechSegmenter&) = delete;

  bool running() const { return active_.load(); }

  bool start();
  void stop();
  void abort();

 private:
  void handlePcm(const std::vector<uint8_t>& pcm);
  void handleSegment(std::vector<uint8_t> pcm, int startSample);
  void transcribeAsync(const SpeechSegment& seg);

  SpeechSegmenterOptions opts_;
  SpeakerStream speaker_;
  FloatRingBuffer ring_;
  std::atomic<int> seq_{0};
  std::atomic<bool> active_{false};
  // Bounded transcription backlog: beyond this, segments are processed
  // synchronously instead of spawning more workers (prevents unbounded thread
  // accumulation during long sessions).
  static constexpr size_t kMaxTranscribeWorkers = 4;
  std::vector<std::thread> workers_;
  std::mutex workersMutex_;
  // Live (not yet finished) worker count; guarded by workersMutex_. Finished
  // workers stay in workers_ (joined at stop) but no longer count against the
  // backlog limit.
  size_t activeWorkers_ = 0;

  // VAD handle (void* to SherpaOnnxVad when AOI_USE_SHERPA).
  void* vad_ = nullptr;

  // Fixed-segment mode: PCM accumulated here, cut every fixedSegmentSeconds.
  std::vector<int16_t> fixedPcm_;
  int64_t fixedStartSample_ = 0;

  // Sliding-window mode: rolling PCM (latest W+Step), cut every Step.
  std::vector<int16_t> slidePcm_;
  int64_t slideStartSample_ = 0;
  int64_t slideNextSample_ = 0;      // next cut position (in samples)
  int64_t slideWindowSamples_ = 0;   // W in samples
  int64_t slideStepSamples_ = 0;     // W-O in samples

  static constexpr size_t CHUNK = 512;  // Silero VAD window at 16k = 32ms
};

} // namespace aoi
