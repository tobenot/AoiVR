// Win7+ APIs (IMFSinkWriter) are hidden by the default Vista SDK target.
// mfreadwrite.h gates on WINVER (not _WIN32_WINNT), so set both.
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include "m4a_encoder.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <wrl/client.h>

namespace aoi {

namespace {

std::wstring toWide(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty()) w.pop_back();
  return w;
}

} // namespace

bool encodeM4a(const std::vector<int16_t>& pcm, uint32_t sampleRate,
               uint32_t channels, const std::string& outPath) {
  if (pcm.empty() || sampleRate == 0 || channels == 0 || channels > 2)
    return false;
  HRESULT hr = S_OK;
  const HRESULT sr = MFStartup(MF_VERSION, 0);
  if (FAILED(sr)) return false;

  bool ok = false;
  do {
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (FAILED(hr = MFCreateAttributes(&attrs, 2))) {
      std::fprintf(stderr, "[m4a] MFCreateAttributes hr=0x%08X\n", hr);
      break;
    }
    if (FAILED(hr = attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                                   MFTranscodeContainerType_MPEG4))) {
      std::fprintf(stderr, "[m4a] SetGUID container hr=0x%08X\n", hr);
      break;
    }

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    if (FAILED(hr = MFCreateSinkWriterFromURL(toWide(outPath).c_str(), nullptr,
                                              attrs.Get(), &writer))) {
      std::fprintf(stderr, "[m4a] SinkWriterFromURL hr=0x%08X\n", hr);
      break;
    }

    // Output: AAC at the captured rate (24 kHz = provider-native MiMo rate).
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    if (FAILED(MFCreateMediaType(&outType))) break;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    // 64 kbps mono: speech stays clean at 24 kHz (12 kHz bandwidth) - far
    // below the provider's 50MB base64 limit even for long turns.
    outType->SetUINT32(MF_MT_AVG_BITRATE, 64000);
    DWORD streamIndex = 0;
    if (FAILED(hr = writer->AddStream(outType.Get(), &streamIndex))) {
      std::fprintf(stderr, "[m4a] AddStream hr=0x%08X\n", hr);
      break;
    }

    // Input: the raw PCM as captured (no resampling needed).
    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) break;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(hr = writer->SetInputMediaType(streamIndex, inType.Get(),
                                              nullptr))) {
      std::fprintf(stderr, "[m4a] SetInputMediaType hr=0x%08X\n", hr);
      break;
    }

    if (FAILED(hr = writer->BeginWriting())) {
      std::fprintf(stderr, "[m4a] BeginWriting hr=0x%08X\n", hr);
      break;
    }

    const UINT64 ticksPerFrame = 10000000ULL / sampleRate;  // 100ns units
    const size_t chunkFrames = static_cast<size_t>(sampleRate) / 4;  // 0.25s
    size_t off = 0;
    UINT64 ts = 0;
    bool writeFailed = false;
    while (off < pcm.size()) {
      const size_t n = std::min(chunkFrames, pcm.size() - off);
      const DWORD byteLen = static_cast<DWORD>(n * channels * sizeof(int16_t));
      Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
      if (FAILED(MFCreateMemoryBuffer(byteLen, &buf))) {
        writeFailed = true;
        break;
      }
      BYTE* ptr = nullptr;
      DWORD maxLen = 0, curLen = 0;
      if (FAILED(buf->Lock(&ptr, &maxLen, &curLen))) {
        writeFailed = true;
        break;
      }
      std::memcpy(ptr, pcm.data() + off, byteLen);
      buf->SetCurrentLength(byteLen);
      buf->Unlock();

      Microsoft::WRL::ComPtr<IMFSample> sample;
      if (FAILED(MFCreateSample(&sample))) {
        writeFailed = true;
        break;
      }
      sample->AddBuffer(buf.Get());
      sample->SetSampleTime(ts);
      sample->SetSampleDuration(static_cast<UINT64>(n) * ticksPerFrame);
      if (FAILED(writer->WriteSample(streamIndex, sample.Get()))) {
        writeFailed = true;
        break;
      }
      off += n;
      ts += static_cast<UINT64>(n) * ticksPerFrame;
    }
    if (writeFailed) break;
    if (FAILED(hr = writer->Finalize())) {
      std::fprintf(stderr, "[m4a] Finalize hr=0x%08X\n", hr);
      break;
    }
    ok = true;
  } while (false);

  MFShutdown();
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
  }
  return ok;
}

} // namespace aoi
