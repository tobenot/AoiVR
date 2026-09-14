#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aoi {

// Captures system speaker output (WASAPI loopback) and streams it as raw
// 16-bit mono PCM at the given sample rate (default 16k). Built on miniaudio's
// ma_device_type_loopback (see THIRD_PARTY_NOTICES.md section 8).
class SpeakerStream {
 public:
  using PcmCallback = std::function<void(const std::vector<uint8_t>& pcm)>;
  using ErrorCallback = std::function<void(const std::string& msg)>;

  SpeakerStream() = default;
  ~SpeakerStream();

  SpeakerStream(const SpeakerStream&) = delete;
  SpeakerStream& operator=(const SpeakerStream&) = delete;

  bool start(PcmCallback onPcm, ErrorCallback onError = nullptr);
  void stop();
  void abort();

  bool running() const { return running_.load(); }

  // Accessors used by the miniaudio data callback (public for the C callback).
  PcmCallback& onPcm() { return onPcm_; }

 private:
  void loop();

  // Serializes start()/stop()/abort() so concurrent calls never race on
  // `thread_` (std::thread is not thread-safe; double-join is UB).
  std::mutex mtx_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  PcmCallback onPcm_;
  ErrorCallback onError_;
  int outRate_ = 16000;
};

} // namespace aoi
