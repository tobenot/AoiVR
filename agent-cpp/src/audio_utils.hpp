#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aoi {

constexpr int SAMPLE_RATE = 16000;

// Convert 16-bit PCM bytes (little-endian) to float samples in [-1, 1].
std::vector<float> pcm16ToFloat32(const uint8_t* buf, size_t len);

// Convert float samples (range [-1, 1]) to 16-bit PCM bytes.
std::vector<uint8_t> float32ToPcm16(const float* samples, size_t n);

// Build a standard 16k mono 16-bit WAV (44-byte header) from raw PCM.
std::vector<uint8_t> buildWavFromPcm(const std::vector<uint8_t>& pcm);

// Build a WAV header for arbitrary sample rate/channels/bits (used by player).
std::vector<uint8_t> buildWavHeader(uint32_t dataLen, uint32_t sampleRate,
                                    uint16_t channels, uint16_t bitsPerSample);

// Convert any 16-bit PCM wav (1-2 channels, any sample rate) to an 8k mono
// 16-bit wav. Uses miniaudio's verified resampler + channel converter
// (ma_resampler / ma_channel_converter) - no hand-written DSP. Returns false
// on unsupported input (non-16-bit, >2 channels, empty).
bool wavTo8kMono(const std::vector<uint8_t>& wav, std::vector<uint8_t>& out);

// A FIFO of float samples fed to the VAD, split into fixed-size chunks.
// Mirrors audio-utils.ts FloatRingBuffer.
class FloatRingBuffer {
 public:
  void push(const float* samples, size_t n);
  // Pop up to n samples (oldest first).
  std::vector<float> take(size_t n);
  size_t length() const { return total_; }
  void reset();

 private:
  std::vector<std::vector<float>> bufs_;
  size_t total_ = 0;
};

} // namespace aoi
