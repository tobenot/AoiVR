#include "audio_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <miniaudio.h>

namespace aoi {

std::vector<float> pcm16ToFloat32(const uint8_t* buf, size_t len) {
  const size_t n = len / 2;
  std::vector<float> out(n);
  for (size_t i = 0; i < n; i++) {
    const int16_t v = static_cast<int16_t>(buf[i * 2] | (buf[i * 2 + 1] << 8));
    out[i] = v / 32768.0f;
  }
  return out;
}

std::vector<uint8_t> float32ToPcm16(const float* samples, size_t n) {
  std::vector<uint8_t> out(n * 2);
  for (size_t i = 0; i < n; i++) {
    int v = static_cast<int>(std::lround(samples[i] * 32768.0f));
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    const int16_t s = static_cast<int16_t>(v);
    out[i * 2] = static_cast<uint8_t>(s & 0xFF);
    out[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
  }
  return out;
}

std::vector<uint8_t> buildWavHeader(uint32_t dataLen, uint32_t sampleRate,
                                    uint16_t channels, uint16_t bitsPerSample) {
  std::vector<uint8_t> header(44, 0);
  const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
  const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
  auto put = [&](size_t off, const char* s) { std::memcpy(header.data() + off, s, 4); };
  auto put32 = [&](size_t off, uint32_t v) {
    header[off] = v & 0xFF; header[off + 1] = (v >> 8) & 0xFF;
    header[off + 2] = (v >> 16) & 0xFF; header[off + 3] = (v >> 24) & 0xFF;
  };
  auto put16 = [&](size_t off, uint16_t v) {
    header[off] = v & 0xFF; header[off + 1] = (v >> 8) & 0xFF;
  };
  put(0, "RIFF");
  put32(4, 36 + dataLen);
  put(8, "WAVE");
  put(12, "fmt ");
  put32(16, 16);
  put16(20, 1);                 // PCM
  put16(22, channels);
  put32(24, sampleRate);
  put32(28, byteRate);
  put16(32, blockAlign);
  put16(34, bitsPerSample);
  put(36, "data");
  put32(40, dataLen);
  return header;
}

std::vector<uint8_t> buildWavFromPcm(const std::vector<uint8_t>& pcm) {
  std::vector<uint8_t> header = buildWavHeader(
      static_cast<uint32_t>(pcm.size()), SAMPLE_RATE, 1, 16);
  header.insert(header.end(), pcm.begin(), pcm.end());
  return header;
}

void FloatRingBuffer::push(const float* samples, size_t n) {
  if (n == 0) return;
  bufs_.emplace_back(samples, samples + n);
  total_ += n;
}

std::vector<float> FloatRingBuffer::take(size_t n) {
  std::vector<float> out;
  out.reserve(n);
  while (out.size() < n && !bufs_.empty()) {
    auto& head = bufs_.front();
    const size_t need = n - out.size();
    if (head.size() <= need) {
      out.insert(out.end(), head.begin(), head.end());
      bufs_.erase(bufs_.begin());
    } else {
      out.insert(out.end(), head.begin(), head.begin() + need);
      head.erase(head.begin(), head.begin() + need);
    }
  }
  total_ = 0;
  for (const auto& b : bufs_) total_ += b.size();
  return out;
}

void FloatRingBuffer::reset() {
  bufs_.clear();
  total_ = 0;
}

bool wavTo8kMono(const std::vector<uint8_t>& wav, std::vector<uint8_t>& out) {
  if (wav.size() <= 44) return false;
  const uint32_t rate = static_cast<uint32_t>(wav[24]) |
                        (static_cast<uint32_t>(wav[25]) << 8) |
                        (static_cast<uint32_t>(wav[26]) << 16) |
                        (static_cast<uint32_t>(wav[27]) << 24);
  const uint16_t ch = static_cast<uint16_t>(wav[22]) |
                      (static_cast<uint16_t>(wav[23]) << 8);
  const uint16_t bits = static_cast<uint16_t>(wav[34]) |
                        (static_cast<uint16_t>(wav[35]) << 8);
  if (bits != 16 || ch == 0 || ch > 2 || rate == 0) return false;
  const size_t frames = (wav.size() - 44) / (static_cast<size_t>(ch) * 2);
  if (frames == 0) return false;
  const auto* pcm = reinterpret_cast<const int16_t*>(wav.data() + 44);

  // 1) Channel conversion (e.g. stereo -> mono, miniaudio mixes channels).
  ma_channel_converter_config ccfg = ma_channel_converter_config_init(
      ma_format_s16, ch, nullptr, 1, nullptr, ma_channel_mix_mode_default);
  ma_channel_converter cc;
  if (ma_channel_converter_init(&ccfg, nullptr, &cc) != MA_SUCCESS) return false;
  std::vector<int16_t> mono(frames);
  const ma_uint64 nFrames = frames;
  const ma_result rc =
      ma_channel_converter_process_pcm_frames(&cc, mono.data(), pcm, nFrames);
  ma_channel_converter_uninit(&cc, nullptr);
  if (rc != MA_SUCCESS) return false;

  // 2) Resampling to 8k (miniaudio's linear-interpolation resampler).
  ma_resampler_config rcfg = ma_resampler_config_init(
      ma_format_s16, 1, rate, 8000, ma_resample_algorithm_linear);
  ma_resampler rs;
  if (ma_resampler_init(&rcfg, nullptr, &rs) != MA_SUCCESS) return false;
  const size_t outCap = frames * 8000 / rate + 8192;
  std::vector<int16_t> outPcm(outCap);
  ma_uint64 consumed = 0, produced = 0;
  bool ok = false;
  while (consumed < frames) {
    ma_uint64 inF = frames - consumed;
    ma_uint64 outF = outCap - produced;
    if (outF == 0) break;
    const ma_result rr = ma_resampler_process_pcm_frames(
        &rs, mono.data() + consumed, &inF, outPcm.data() + produced, &outF);
    if (rr != MA_SUCCESS) break;
    consumed += inF;
    produced += outF;
    if (inF == 0 && outF == 0) break;
  }
  ma_resampler_uninit(&rs, nullptr);
  if (consumed < frames || produced == 0) return false;

  const auto header =
      buildWavHeader(static_cast<uint32_t>(produced * 2), 8000, 1, 16);
  out.clear();
  out.reserve(header.size() + produced * 2);
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(outPcm.data()),
             reinterpret_cast<const uint8_t*>(outPcm.data()) + produced * 2);
  return true;
}

} // namespace aoi
