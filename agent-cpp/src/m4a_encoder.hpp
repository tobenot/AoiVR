#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aoi {

// Encode 16-bit PCM audio to an M4A (AAC) file using Media Foundation's
// SinkWriter - the Windows-sanctioned high-level pipeline. It auto-selects
// the built-in AAC encoder MFT and the MP4 muxer, so no manual MFT
// negotiation, no frame alignment and no container code is written here.
// AAC-LC accepts any rate in 8-96 kHz, so the 24 kHz mic capture (the
// provider's native MiMo-Audio rate) is encoded directly - no resampling.
// Returns false on any WMF failure (caller falls back to sending wav).
bool encodeM4a(const std::vector<int16_t>& pcm, uint32_t sampleRate,
               uint32_t channels, const std::string& outPath);

} // namespace aoi
