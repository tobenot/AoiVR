#include "speaker.hpp"

#include <chrono>
#include <cstring>

#include <miniaudio.h>

namespace aoi {

namespace {
constexpr int kMiniaudioConvertSize = 4096;

struct LoopbackCtx {
  SpeakerStream* self = nullptr;
  std::vector<uint8_t> pcm;
};

void loopbackCallback(ma_device* device, void* /*pOutput*/, const void* pInput,
                      ma_uint32 frameCount) {
  auto* ctx = static_cast<LoopbackCtx*>(device->pUserData);
  if (!ctx || !ctx->self) return;
  if (!pInput || frameCount == 0) return;

  // miniaudio converts the loopback stream to the configured format
  // (ma_format_s16, mono, outRate_) automatically, so pInput is already
  // 16-bit mono PCM.
  const size_t bytes = static_cast<size_t>(frameCount) * sizeof(int16_t);
  ctx->pcm.assign(static_cast<const uint8_t*>(pInput),
                  static_cast<const uint8_t*>(pInput) + bytes);
  if (ctx->self->onPcm()) ctx->self->onPcm()(ctx->pcm);
}

} // namespace

SpeakerStream::~SpeakerStream() { abort(); }

bool SpeakerStream::start(PcmCallback onPcm, ErrorCallback onError) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (running_.load()) return false;
  // Join any leftover thread from a previous start() whose loop() exited early
  // (e.g. ma_device_init_ex failed): reassigning a joinable std::thread calls
  // std::terminate().
  if (thread_.joinable()) thread_.join();
  onPcm_ = std::move(onPcm);
  onError_ = std::move(onError);
  running_ = true;
  stopRequested_ = false;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void SpeakerStream::loop() {
  ma_backend backends[] = {ma_backend_wasapi, ma_backend_winmm};

  ma_device_config config = ma_device_config_init(ma_device_type_loopback);
  config.capture.format = ma_format_s16;
  config.capture.channels = 1;
  config.sampleRate = static_cast<ma_uint32>(outRate_);
  config.dataCallback = loopbackCallback;
  config.periodSizeInFrames = 0;
  config.performanceProfile = ma_performance_profile_low_latency;

  LoopbackCtx ctx;
  ctx.self = this;
  config.pUserData = &ctx;

  ma_device device;
  ma_result result = ma_device_init_ex(backends, sizeof(backends) / sizeof(backends[0]),
                                       nullptr, &config, &device);
  if (result != MA_SUCCESS) {
    if (onError_) onError_("WASAPI loopback init failed");
    running_ = false;
    return;
  }

  result = ma_device_start(&device);
  if (result != MA_SUCCESS) {
    if (onError_) onError_("WASAPI loopback start failed");
    ma_device_uninit(&device);
    running_ = false;
    return;
  }

  while (running_.load() && !stopRequested_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ma_device_uninit(&device);
  running_ = false;
}

void SpeakerStream::stop() {
  std::lock_guard<std::mutex> lk(mtx_);
  if (!running_.load()) {
    // A previous start() that failed inside loop() leaves the thread joinable;
    // join it so the next start() doesn't reassign a live std::thread.
    if (thread_.joinable()) thread_.join();
    return;
  }
  stopRequested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void SpeakerStream::abort() {
  std::lock_guard<std::mutex> lk(mtx_);
  stopRequested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

} // namespace aoi
