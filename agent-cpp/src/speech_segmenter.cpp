#include "speech_segmenter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef AOI_USE_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

#include "sherpa_asr.hpp"

namespace aoi {

namespace {

#ifdef AOI_USE_SHERPA
const char* getVadModelPath() {
  const char* env = getenv("SHERPA_VAD_MODEL");
  if (env) return env;
  return "models/sherpa/silero_vad.onnx";
}
#endif

} // namespace

SpeechSegmenter::SpeechSegmenter(SpeechSegmenterOptions opts) : opts_(std::move(opts)) {}

SpeechSegmenter::~SpeechSegmenter() { abort(); }

bool SpeechSegmenter::start() {
  if (active_.load()) return true;
  active_ = true;

#ifdef AOI_USE_SHERPA
  SherpaOnnxSileroVadModelConfig silero{};
  silero.model = getVadModelPath();
  silero.threshold = 0.5;
  silero.min_silence_duration = 0.5;
  silero.min_speech_duration = 0.15;
  silero.max_speech_duration = 5;
  silero.window_size = 512;

  SherpaOnnxVadModelConfig cfg{};
  cfg.silero_vad_model_config = silero;
  cfg.sample_rate = "16000";
  cfg.num_threads = 1;
  cfg.provider = "cpu";
  cfg.debug = 0;

  vad_ = SherpaOnnxCreateVad(&cfg);
  if (!vad_) {
    active_ = false;
    if (opts_.onState) opts_.onState("error", "VAD init failed");
    return false;
  }
#else
  // Without sherpa-onnx, no VAD: every chunk boundary becomes a segment
  // placeholder is disabled; we simply don't segment (audio is dropped).
#endif

  seq_ = 0;
  ring_.reset();
  // Reset sliding-window state: the absolute sample counters are relative to
  // "start()" and MUST be re-initialized on restart, otherwise the capture
  // frontier can never catch up with the stale slideNextSample_ and the
  // window stops cutting forever (and the first cut could underflow the
  // buffer index).
  slidePcm_.clear();
  slideStartSample_ = 0;
  slideNextSample_ = 0;
  slideStepSamples_ = 0;
  slideWindowSamples_ = 0;
  fixedPcm_.clear();
  fixedStartSample_ = 0;

  speaker_.start(
      [this](const std::vector<uint8_t>& pcm) { handlePcm(pcm); },
      [this](const std::string& msg) {
        if (opts_.onState) opts_.onState("error", msg);
      });

  if (opts_.onState) opts_.onState("started", "");
  return true;
}

void SpeechSegmenter::handlePcm(const std::vector<uint8_t>& pcm) {
  if (!active_.load()) return;

  // Sliding-window mode: rolling buffer of the latest W+Step seconds, emit the
  // latest W every Step seconds (overlapping windows).
  if (opts_.windowSeconds > 0) {
    if (slideWindowSamples_ == 0) {
      slideWindowSamples_ = static_cast<int64_t>(opts_.windowSeconds) * SAMPLE_RATE;
      const int64_t stepMs = static_cast<int64_t>(opts_.windowSeconds) * 1000 - opts_.overlapSeconds;
      slideStepSamples_ = std::max<int64_t>(SAMPLE_RATE / 2, stepMs * SAMPLE_RATE / 1000);
      slideNextSample_ = slideWindowSamples_;  // first cut after a full window
    }
    const size_t n = pcm.size() / 2;
    for (size_t i = 0; i < n; ++i) {
      int16_t v;
      memcpy(&v, pcm.data() + i * 2, 2);
      slidePcm_.push_back(v);
    }
    // Keep only W+Step samples (ring behavior, never grows unbounded).
    const int64_t keep = slideWindowSamples_ + slideStepSamples_;
    if (static_cast<int64_t>(slidePcm_.size()) > keep) {
      const int64_t eraseN = static_cast<int64_t>(slidePcm_.size()) - keep;
      slidePcm_.erase(slidePcm_.begin(), slidePcm_.begin() + eraseN);
      slideStartSample_ += eraseN;
    }
    // Cut windows when the CAPTURE FRONTIER (absolute sample count at the
    // buffer tail) reaches the target. slideNextSample_ is absolute-from-start;
    // the buffer itself is a trimmed rolling window (never longer than W+Step),
    // so comparing against slidePcm_.size() would stop cutting forever once
    // slideNextSample_ exceeds the trimmed buffer length (only the first two
    // windows would ever fire).
    const int64_t frontier = slideStartSample_ + static_cast<int64_t>(slidePcm_.size());
    while (slideNextSample_ <= frontier) {
      // Window k ends exactly at slideNextSample_ (its scheduled cut point),
      // not at the current buffer tail. When one late/large chunk carries the
      // frontier past several step boundaries, ending every window at the tail
      // would emit duplicate audio and never cover the middle positions.
      const int64_t winEnd = slideNextSample_;
      int64_t winStart = winEnd - slideWindowSamples_;
      if (winStart < slideStartSample_) winStart = slideStartSample_;
      const int64_t s0 = winStart - slideStartSample_;
      const int64_t s1 = winEnd - slideStartSample_;
      std::vector<uint8_t> pcm16;
      pcm16.reserve(static_cast<size_t>(s1 - s0) * 2);
      for (int64_t i = s0; i < s1; ++i) {
        uint8_t b[2];
        memcpy(b, &slidePcm_[i], 2);
        pcm16.push_back(b[0]);
        pcm16.push_back(b[1]);
      }
      handleSegment(std::move(pcm16), static_cast<int>(winStart));
      slideNextSample_ += slideStepSamples_;
    }
    return;
  }

  // Fixed-segment mode: accumulate raw PCM, cut every fixedSegmentSeconds.
  if (opts_.fixedSegmentSeconds > 0) {
    const int64_t target = static_cast<int64_t>(opts_.fixedSegmentSeconds) * SAMPLE_RATE;
    const size_t n = pcm.size() / 2;
    for (size_t i = 0; i < n; ++i) {
      int16_t v;
      memcpy(&v, pcm.data() + i * 2, 2);
      fixedPcm_.push_back(v);
    }
    if (fixedPcm_.empty()) return;
    if (static_cast<int64_t>(fixedPcm_.size()) >= target) {
      std::vector<uint8_t> pcm16;
      pcm16.reserve(fixedPcm_.size() * 2);
      for (int16_t v : fixedPcm_) {
        uint8_t b[2];
        memcpy(b, &v, 2);
        pcm16.push_back(b[0]);
        pcm16.push_back(b[1]);
      }
      handleSegment(std::move(pcm16), static_cast<int>(fixedStartSample_));
      fixedStartSample_ += static_cast<int64_t>(fixedPcm_.size());
      fixedPcm_.clear();
    }
    return;
  }

#ifdef AOI_USE_SHERPA
  if (!vad_) return;
#endif
  const auto floats = pcm16ToFloat32(pcm.data(), pcm.size());
  ring_.push(floats.data(), floats.size());

  while (ring_.length() >= CHUNK) {
    auto chunk = ring_.take(CHUNK);
#ifdef AOI_USE_SHERPA
    SherpaOnnxVadAcceptWaveform(vad_, chunk.data(), static_cast<int>(chunk.size()));
    // Drain completed segments.
    while (!SherpaOnnxVadEmpty(vad_)) {
      const SherpaOnnxSpeechSegment* seg = SherpaOnnxVadFront(vad_);
      if (!seg || seg->n == 0) {
        SherpaOnnxVadPop(vad_);
        continue;
      }
      std::vector<float> samples(seg->samples, seg->samples + seg->n);
      const int start = seg->start;
      SherpaOnnxVadPop(vad_);
      auto pcm16 = float32ToPcm16(samples.data(), samples.size());
      handleSegment(std::move(pcm16), start);
    }
#endif
  }
}

void SpeechSegmenter::handleSegment(std::vector<uint8_t> pcm, int startSample) {
  if (!active_.load()) return;
  const auto wav = buildWavFromPcm(pcm);
  const int durationMs = static_cast<int>(std::lround(pcm.size() / 2.0 / (SAMPLE_RATE / 1000.0)));
  const int seq = ++seq_;

  SpeechSegment seg;
  seg.wavBuffer = wav;
  seg.seq = seq;
  seg.durationMs = durationMs;
  seg.startSample = startSample;
  if (opts_.windowSeconds > 0) {
    seg.sliding = true;
    seg.windowStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  }

  // Sliding-window / fixed-segment mode: raw audio for on-demand AI consumption,
  // or (interpretation) audio-LLM translation. No local transcription.
  if (opts_.windowSeconds > 0 || opts_.fixedSegmentSeconds > 0) {
    if (opts_.onSegment) opts_.onSegment(seg);
    return;
  }
  transcribeAsync(seg);
}

void SpeechSegmenter::transcribeAsync(const SpeechSegment& seg) {
  // Bound the worker backlog: each segment spawns a thread stored in
  // workers_ (only joined at stop), so a long session would accumulate
  // threads and OS handles forever. Count LIVE workers (completed ones are
  // removed) - a size-only check would permanently fall through to the
  // synchronous path after the first kMax workers finished. When the backlog
  // is full, process the segment synchronously instead of spawning another
  // worker.
  bool spawned = false;
  {
    std::lock_guard<std::mutex> lk(workersMutex_);
    if (activeWorkers_ < kMaxTranscribeWorkers) {
      ++activeWorkers_;
      spawned = true;
      workers_.emplace_back([this, seg]() {
        if (!active_.load()) return;
#ifdef AOI_USE_SHERPA
        const auto floats = pcm16ToFloat32(seg.wavBuffer.data() + 44, seg.wavBuffer.size() - 44);
        const auto r = sherpaTranscribeFull(floats);
        if (active_.load()) {
          SpeechSegment out = seg;
          out.text = r.text;
          out.language = r.language;
          if (opts_.onSegment) opts_.onSegment(out);
        }
#else
        // No sherpa: emit the segment without transcription (audio understanding path).
        if (opts_.onSegment) opts_.onSegment(seg);
#endif
        // Decrement the live count on a separate thread-safe path: joining
        // ourselves (e.g. if the callback triggered stop()) would deadlock.
        {
          std::lock_guard<std::mutex> lk2(workersMutex_);
          if (activeWorkers_ > 0) --activeWorkers_;
        }
      });
    }
  }
  if (spawned) return;
  // Backlog full: synchronous fallback (same path the worker would take).
  if (opts_.onSegment) opts_.onSegment(seg);
}

void SpeechSegmenter::stop() {
  if (!active_.load()) return;
  active_ = false;
  speaker_.stop();
  ring_.reset();
  fixedPcm_.clear();
  slidePcm_.clear();
#ifdef AOI_USE_SHERPA
  if (vad_) {
    SherpaOnnxDestroyVad(vad_);
    vad_ = nullptr;
  }
#endif
  // Move the threads out under the lock, then join OUTSIDE it. If stop() was
  // called from INSIDE an onSegment callback (i.e. from a worker thread
  // itself), joining self would deadlock/throw: detach that one instead -
  // active_ is already false so it exits after the callback returns.
  std::vector<std::thread> local;
  {
    std::lock_guard<std::mutex> lk(workersMutex_);
    local.swap(workers_);
    activeWorkers_ = 0;
  }
  const auto selfId = std::this_thread::get_id();
  for (auto& t : local) {
    if (!t.joinable()) continue;
    if (t.get_id() == selfId) t.detach();
    else t.join();
  }
  if (opts_.onState) opts_.onState("stopped", "");
}

void SpeechSegmenter::abort() {
  active_ = false;
  speaker_.abort();
#ifdef AOI_USE_SHERPA
  if (vad_) {
    SherpaOnnxDestroyVad(vad_);
    vad_ = nullptr;
  }
#endif
  // Same pattern as stop(): move the threads out under the lock, join
  // OUTSIDE it. Joining while holding workersMutex_ deadlocks - each worker
  // takes that mutex to decrement activeWorkers_ before exiting.
  std::vector<std::thread> local;
  {
    std::lock_guard<std::mutex> lk(workersMutex_);
    local.swap(workers_);
    activeWorkers_ = 0;
  }
  const auto selfId = std::this_thread::get_id();
  for (auto& t : local) {
    if (!t.joinable()) continue;
    // Same self-join guard as stop(): an onSegment callback calling abort()
    // from a worker thread must not join itself.
    if (t.get_id() == selfId) t.detach();
    else t.join();
  }
}

} // namespace aoi
