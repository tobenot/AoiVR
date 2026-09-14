// Verify WMF mp3 encoding: read speech_long_16k.wav, extract PCM, encode to
// mp3, report success/size. Dev tool.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "mp3_encoder.hpp"

int main(int argc, char** argv) {
  // Optional: encode a 44.1k/48k wav passed on the command line, then stop.
  if (argc > 1) {
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
      std::fprintf(stderr, "cannot open %s\n", argv[1]);
      return 1;
    }
    std::vector<uint8_t> wav((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (wav.size() <= 44) return 1;
    uint32_t rate = 0;
    for (int i = 0; i < 4; ++i) rate |= static_cast<uint32_t>(wav[24 + i]) << (8 * i);
    const uint16_t channels = static_cast<uint16_t>(wav[22] | (wav[23] << 8));
    std::vector<int16_t> pcm((wav.size() - 44) / 2);
    std::memcpy(pcm.data(), wav.data() + 44, pcm.size() * 2);
    const std::string out = std::string(argv[1]) + ".mp3";
    const bool ok = aoi::encodeMp3(pcm, rate, channels, out);
    std::printf("encode %s -> %s: %s\n", argv[1], out.c_str(), ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
  }

  std::ifstream f("speech_long_16k.wav", std::ios::binary);
  if (!f) {
    std::puts("no speech_long_16k.wav");
    return 1;
  }
  std::vector<uint8_t> wav((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
  if (wav.size() <= 44) {
    std::puts("wav too small");
    return 1;
  }
  // Parse wav header: sample rate at offset 24, channels at 22, bits at 34.
  uint32_t rate = 0;
  for (int i = 0; i < 4; ++i) rate |= static_cast<uint32_t>(wav[24 + i]) << (8 * i);
  const uint16_t channels = static_cast<uint16_t>(wav[22] | (wav[23] << 8));
  const uint16_t bits = static_cast<uint16_t>(wav[34] | (wav[35] << 8));
  std::printf("wav: rate=%u ch=%u bits=%u data=%zu bytes\n", rate, channels,
              bits, wav.size() - 44);

  std::vector<int16_t> pcm((wav.size() - 44) / 2);
  std::memcpy(pcm.data(), wav.data() + 44, pcm.size() * 2);

  std::string out = "wav_encoded_test.mp3";
  // NOTE: the WMF MP3 encoder needs a 44.1k/48k input; the test file is 16k,
  // so this test exercises the fast-fail path. For a real encode, feed 44.1k.
  const bool ok16 = aoi::encodeMp3(pcm, rate, channels, out);
  std::printf("encode@%uHz: %s\n", rate, ok16 ? "OK" : "rejected (expected for non-44.1k)");

  // Real encode: synthesize 4s of 44.1k silence.
  const uint32_t r44 = 44100;
  std::vector<int16_t> pcm44(4 * r44, 0);
  const bool ok44 = aoi::encodeMp3(pcm44, r44, 1, out);
  if (ok44) {
    LARGE_INTEGER sz{};
    HANDLE h = CreateFileA(out.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      GetFileSizeEx(h, &sz);
      CloseHandle(h);
    }
    std::printf("encode@44.1k: OK, mp3 size=%lld\n", sz.QuadPart);
    return 0;
  }
  std::puts("encode@44.1k: FAILED");
  return 1;
}
