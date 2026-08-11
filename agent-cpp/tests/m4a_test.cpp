// Media Foundation SinkWriter AAC/M4A encoder test: synthesize 1s of 24k
// silence (the provider-native rate), encode to .m4a, print the file size.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "m4a_encoder.hpp"

int main(int argc, char** argv) {
  const uint32_t rate = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 24000;
  std::vector<int16_t> pcm(rate, 0);  // 1s of silence, mono
  const std::string out = "test_24k.m4a";
  const bool ok = aoi::encodeM4a(pcm, rate, 1, out);
  if (!ok) {
    std::printf("encodeM4a@%u: FAILED\n", rate);
    return 1;
  }
  LARGE_INTEGER sz{};
  HANDLE h = CreateFileA(out.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
  }
  std::printf("encodeM4a@%u: OK, m4a size=%lld\n", rate, sz.QuadPart);
  return 0;
}
