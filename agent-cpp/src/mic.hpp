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
  // Capture sample rate. 24000 Hz is the provider's native MiMo-Audio
  // tokenizer rate ("sampling_rate": 24000): audio is re-resampled to 24k
  // server-side anyway, and FLAC encoding (miniaudio ma_encoder) accepts any
  // rate - so capturing at 24k needs zero resampling anywhere in the
  // pipeline, and the provider decodes it at its native rate.
  int sampleRate = 24000;
};
// Records microphone audio via miniaudio ma_device_type_capture (see
// THIRD_PARTY_NOTICES.md section 8) into a WAV buffer. Mirrors mic.ts.
class MicCapture {
 public:
  MicCapture() = default;
  ~MicCapture();

  MicCapture(const MicCapture&) = delete;
  MicCapture& operator=(const MicCapture&) = delete;

  bool start(int sampleRate = 24000);
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
  int sampleRate_ = 24000;
  // PCM bytes: 16-bit LE, mono, at sampleRate_. WASAPI shared mode delivers
  // exactly this format (engine handles resampling/mixing), which FLAC
  // encodes directly - no conversion anywhere.
  std::vector<uint8_t> pcm_;
  std::mutex resultMutex_;
};

} // namespace aoi
