#include "player.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include <miniaudio.h>

namespace aoi {

namespace {

struct PlaybackState {
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::atomic<bool> active{false};
  std::mutex mutex;
};

PlaybackState& playback() {
  static PlaybackState p;
  return p;
}

struct PlayCtx {
  const std::vector<uint8_t>* pcm = nullptr;
  std::atomic<size_t> pos{0};  // written by the audio callback, read by the play loop
  std::atomic<bool>* stopping = nullptr;
};

void playCallback(ma_device* device, void* pOutput, const void* /*pInput*/,
                  ma_uint32 frameCount) {
  auto* ctx = static_cast<PlayCtx*>(device->pUserData);
  if (!ctx) return;
  const size_t bytes = static_cast<size_t>(frameCount) * sizeof(int16_t);
  uint8_t* out = static_cast<uint8_t*>(pOutput);
  if (ctx->stopping && ctx->stopping->load()) {
    std::memset(out, 0, bytes);
    return;
  }
  if (ctx->pos >= ctx->pcm->size()) {
    std::memset(out, 0, bytes);
    return;
  }
  const size_t remaining = ctx->pcm->size() - ctx->pos;
  const size_t copyBytes = remaining < bytes ? remaining : bytes;
  std::memcpy(out, ctx->pcm->data() + ctx->pos, copyBytes);
  if (copyBytes < bytes) std::memset(out + copyBytes, 0, bytes - copyBytes);
  ctx->pos += copyBytes;
}

} // namespace

// Plays 16-bit PCM through the default output device using miniaudio. This
// call BLOCKS until playback finishes (or stopPlayback() interrupts), matching
// the JS playPcm16() await — so the caller only proceeds to the next sentence
// after the current one has fully played.
void playPcm16(const std::vector<uint8_t>& pcm, int sampleRate) {
  if (pcm.empty()) return;
  auto& p = playback();

  // Serialize the whole stop-old -> take-ownership sequence: two concurrent
  // playPcm16 calls could both observe active==false and both take ownership
  // (overlapping devices, mutual interruption via the shared stopping flag).
  std::unique_lock<std::mutex> own(p.mutex);
  // Interrupt any previous playback and wait for it to finish.
  p.stopping = true;
  while (p.active.load()) {
    own.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    own.lock();
  }

  p.active = true;
  p.stopping = false;

  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_s16;
  config.playback.channels = 1;
  config.sampleRate = static_cast<ma_uint32>(sampleRate);
  config.dataCallback = playCallback;
  config.periodSizeInFrames = 0;
  config.performanceProfile = ma_performance_profile_low_latency;

  PlayCtx ctx;
  ctx.pcm = &pcm;
  ctx.stopping = &p.stopping;
  config.pUserData = &ctx;

  ma_device device;
  const ma_result initRes = ma_device_init(nullptr, &config, &device);
  if (initRes != MA_SUCCESS) {
    fprintf(stderr, "[Play] ma_device_init failed: %d (pcm=%zu sr=%d)\n",
            static_cast<int>(initRes), pcm.size(), sampleRate);
    p.active = false;
    return;
  }
  const ma_result startRes = ma_device_start(&device);
  if (startRes != MA_SUCCESS) {
    fprintf(stderr, "[Play] ma_device_start failed: %d (pcm=%zu sr=%d)\n",
            static_cast<int>(startRes), pcm.size(), sampleRate);
    ma_device_uninit(&device);
    p.active = false;
    return;
  }
  // Another thread may have called stopPlayback() while we initialized the
  // device; honor it immediately instead of playing the first buffer.
  own.unlock();

  // Block until fully played or a stop is requested.
  while (!p.stopping.load()) {
    if (ctx.pos >= pcm.size()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ma_device_uninit(&device);
  p.active = false;
}

void stopPlayback() {
  auto& p = playback();
  p.stopping = true;
}

bool isPlaying() { return playback().active.load(); }

} // namespace aoi
