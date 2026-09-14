#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aoi {

// Encode 16-bit PCM audio to an MP3 file using Windows Media Foundation (the
// built-in MP3 encoder MFT - no external dependencies). The mic captures at
// 44100 Hz directly, which is a native input rate for the encoder, so no
// resampling is needed. Returns false on any WMF failure (caller falls back
// to sending the raw wav).
bool encodeMp3(const std::vector<int16_t>& pcm, uint32_t sampleRate,
               uint32_t channels, const std::string& outPath);

} // namespace aoi
