#include "mic.hpp"

#include <chrono>

#include <miniaudio.h>

#include "audio_utils.hpp"

namespace aoi {

namespace {
struct MicCtx {
  MicCapture* self = nullptr;
  ma_uint32 channels = 2;
};

void captureCallback(ma_device* device, void* pOutput, const void* pInput,
                     ma_uint32 frameCount) {
  (void)pOutput;
  auto* ctx = static_cast<MicCtx*>(device->pUserData);
  if (!ctx || !ctx->self) return;
  if (!pInput || frameCount == 0) return;
  // Configured ma_format_s16 with N channels at sampleRate_. WASAPI shared
  // mode lets the Windows audio engine do all resampling/channel mixing, so
  // the callback delivers exactly what we asked for (44.1k stereo s16).
  const size_t bytes = static_cast<size_t>(frameCount) * ctx->channels *
                       sizeof(int16_t);
  std::lock_guard<std::mutex> lk(ctx->self->resultMutex());
  auto& pcm = ctx->self->pcm();
  pcm.insert(pcm.end(), static_cast<const uint8_t*>(pInput),
             static_cast<const uint8_t*>(pInput) + bytes);
}

} // namespace

MicCapture::~MicCapture() { abort(); }

bool MicCapture::start(int sampleRate) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (running_.load()) return false;
  // Join any leftover thread from a previous start() whose recordLoop exited
  // early (e.g. ma_device_init failed without a device present): the flag is
  // already false but the std::thread is still joinable, and reassigning it
  // would call std::terminate().
  if (thread_.joinable()) thread_.join();
  sampleRate_ = sampleRate;
  running_ = true;
  stopRequested_ = false;
  {
    // Clear any leftover buffer from a previous recording so a new turn never
    // starts with stale audio appended (V2: mic buffer cross-turn pollution).
    std::lock_guard<std::mutex> lk(resultMutex_);
    pcm_.clear();
  }
  thread_ = std::thread([this] { recordLoop(); });
  return true;
}

void MicCapture::recordLoop() {
  ma_device_config config = ma_device_config_init(ma_device_type_capture);
  config.capture.format = ma_format_s16;
  config.capture.channels = 2;
  config.sampleRate = static_cast<ma_uint32>(sampleRate_);
  config.dataCallback = captureCallback;

  MicCtx ctx;
  ctx.self = this;
  ctx.channels = 2;
  config.pUserData = &ctx;

  ma_device device;
  ma_result result = ma_device_init(nullptr, &config, &device);
  if (result != MA_SUCCESS) {
    running_ = false;
    return;
  }

  result = ma_device_start(&device);
  if (result != MA_SUCCESS) {
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

MicResult MicCapture::stop() {
  std::lock_guard<std::mutex> lk(mtx_);
  if (!running_.load()) {
    // A previous start() that failed inside recordLoop() leaves the thread
    // joinable; join it here so the next start() doesn't reassign a live
    // std::thread (std::terminate).
    if (thread_.joinable()) thread_.join();
    return {};
  }
  stopRequested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
  std::lock_guard<std::mutex> lk2(resultMutex_);
  MicResult res;
  std::vector<uint8_t> header =
      buildWavHeader(static_cast<uint32_t>(pcm_.size()), sampleRate_, 2, 16);
  res.wavBuffer = header;
  res.wavBuffer.insert(res.wavBuffer.end(), pcm_.begin(), pcm_.end());
  res.sampleRate = sampleRate_;
  pcm_.clear();
  return res;
}

void MicCapture::abort() {
  std::lock_guard<std::mutex> lk(mtx_);
  stopRequested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
  {
    std::lock_guard<std::mutex> lk2(resultMutex_);
    pcm_.clear();
  }
}

} // namespace aoi
