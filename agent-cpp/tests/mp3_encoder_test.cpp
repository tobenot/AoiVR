// WMF mp3 encoder feasibility test: read a 16k mono wav, encode to mp3,
// verify a valid mp3 is produced (and print sizes).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "mp3_encoder.hpp"

int main(int argc, char** argv) {
  const std::string in = argc > 1 ? argv[1] : "speech_long_16k.wav";
  const std::string out = argc > 2 ? argv[2] : "out_encoded.mp3";

  std::ifstream f(in, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", in.c_str());
    return 1;
  }
  std::vector<uint8_t> wav((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
  if (wav.size() <= 44) {
    std::fprintf(stderr, "wav too small\n");
    return 1;
  }
  const uint32_t rate = static_cast<uint32_t>(wav[24]) |
                        (static_cast<uint32_t>(wav[25]) << 8) |
                        (static_cast<uint32_t>(wav[26]) << 16) |
                        (static_cast<uint32_t>(wav[27]) << 24);
  const size_t pcmBytes = wav.size() - 44;
  const size_t sampleCount = pcmBytes / 2;
  const auto mp3 = aoi::encodePcmToMp3(
      reinterpret_cast<const int16_t*>(wav.data() + 44), sampleCount, rate);

  if (mp3.empty()) {
    std::fprintf(stderr, "mp3 encode FAILED\n");
    return 1;
  }
  std::ofstream o(out, std::ios::binary);
  o.write(reinterpret_cast<const char*>(mp3.data()), mp3.size());
  o.close();
  std::printf("OK: rate=%u samples=%zu wav=%zu -> mp3=%zu bytes\n", rate,
              sampleCount, wav.size(), mp3.size());
  return 0;
}
