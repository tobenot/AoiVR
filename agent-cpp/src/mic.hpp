#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aoi {

struct MicResult {
  std::vector<uint8_t> wavBuffer;  // complete 44-byte header + PCM
  int sampleRate = 16000;
};

// Records microphone audio via miniaudio ma_device_type_capture (see
// THIRD_PARTY_NOTICES.md section 8) into a WAV buffer. Mirrors mic.ts.
class MicCapture {
 public:
  MicCapture() = default;
  ~MicCapture();

  MicCapture(const MicCapture&) = delete;
  MicCapture& operator=(const MicCapture&) = delete;

  bool start(int sampleRate = 16000);
  MicResult stop();
  void abort();

  bool running() const { return running_.load(); }

  // Accessors used by the miniaudio data callback (public for the C callback).
  std::mutex& resultMutex() { return resultMutex_; }
  std::vector<uint8_t>& pcm() { return pcm_; }

 private:
  void recordLoop();

  std::thread thread_;
  // Serializes start()/stop()/abort() so concurrent calls (e.g. the message
  // worker starting a capture while the host thread stops the agent) never
  // race on `thread_` / flags (std::thread is not thread-safe).
  std::mutex mtx_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  int sampleRate_ = 16000;
  std::vector<uint8_t> pcm_;
  std::mutex resultMutex_;
};

} // namespace aoi
