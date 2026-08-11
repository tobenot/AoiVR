#include "mp3_encoder.hpp"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <wrl/client.h>

namespace aoi {

namespace {

std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.pop_back();
  return w;
}

// Process MP3 output samples out of the encoder until it needs more input.
// Returns false on real failures (NEED_MORE_INPUT is normal).
bool drainOutput(IMFTransform* enc, FILE* f, bool* wroteAny) {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER odb{};
    odb.dwStreamID = 0;
    Microsoft::WRL::ComPtr<IMFSample> out;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> ob;
    DWORD status = 0;
    if (FAILED(MFCreateSample(&out))) return false;
    if (FAILED(MFCreateMemoryBuffer(16384, &ob))) return false;
    if (FAILED(out->AddBuffer(ob.Get()))) return false;
    odb.pSample = out.Get();
    const HRESULT hr = enc->ProcessOutput(0, 1, &odb, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
    if (FAILED(hr)) return false;
    // Write the produced MP3 bytes.
    IMFMediaBuffer* mb = nullptr;
    if (odb.pSample) odb.pSample->GetBufferByIndex(0, &mb);
    if (mb) {
      BYTE* ptr = nullptr;
      DWORD cur = 0, max = 0;
      if (SUCCEEDED(mb->Lock(&ptr, &max, &cur)) && cur > 0) {
        if (f) std::fwrite(ptr, 1, cur, f);
        if (wroteAny) *wroteAny = true;
        mb->Unlock();
      }
    }
  }
}

} // namespace

bool encodeMp3(const std::vector<int16_t>& pcm, uint32_t sampleRate,
               uint32_t channels, const std::string& outPath) {
  if (pcm.empty() || sampleRate == 0 || channels == 0) return false;
  // The built-in MP3 encoder MFT accepts 44100/48000 Hz input.
  if (sampleRate != 44100 && sampleRate != 48000) return false;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
  const bool coInit = hr == S_OK;

  Microsoft::WRL::ComPtr<IMFTransform> enc;
  // Find the built-in MP3 encoder MFT: PCM in -> MP3 out.
  MFT_REGISTER_TYPE_INFO inT = {MFMediaType_Audio, MFAudioFormat_PCM};
  MFT_REGISTER_TYPE_INFO outT = {MFMediaType_Audio, MFAudioFormat_MP3};
  IMFActivate** acts = nullptr;
  UINT32 n = 0;
  hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, &inT, &outT,
                 &acts, &n);
  if (FAILED(hr) || n == 0) {
    if (coInit) CoUninitialize();
    return false;
  }
  hr = acts[0]->ActivateObject(IID_PPV_ARGS(&enc));
  for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
  CoTaskMemFree(acts);
  if (FAILED(hr)) {
    if (coInit) CoUninitialize();
    return false;
  }

  // Output type FIRST (MFT negotiation order for encoders: downstream first).
  Microsoft::WRL::ComPtr<IMFMediaType> out;
  for (DWORD i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IMFMediaType> t;
    if (FAILED(enc->GetOutputAvailableType(0, i, &t))) break;
    GUID sub{};
    UINT32 sr = 0;
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    if (sub == MFAudioFormat_MP3 && sr == sampleRate) {
      out = t;
      break;
    }
  }
  if (!out) {
    if (coInit) CoUninitialize();
    return false;
  }
  hr = enc->SetOutputType(0, out.Get(), 0);
  if (FAILED(hr)) {
    if (coInit) CoUninitialize();
    return false;
  }

  // Input: 16-bit PCM at the captured sample rate. Enumerate the encoder's
  // accepted input types and pick a matching one (channel count may differ).
  Microsoft::WRL::ComPtr<IMFMediaType> in;
  UINT32 inChannels = channels;
  for (DWORD i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IMFMediaType> t;
    if (FAILED(enc->GetInputAvailableType(0, i, &t))) break;
    GUID sub{};
    UINT32 sr = 0, ch = 0, bits = 0;
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (sub == MFAudioFormat_PCM && sr == sampleRate && bits == 16) {
      in = t;
      inChannels = ch;
      break;
    }
  }
  if (!in) {
    if (coInit) CoUninitialize();
    return false;
  }
  hr = enc->SetInputType(0, in.Get(), 0);
  if (FAILED(hr)) {
    if (coInit) CoUninitialize();
    return false;
  }

  if (FAILED(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
      FAILED(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
    if (coInit) CoUninitialize();
    return false;
  }

  FILE* f = _wfopen(toWide(outPath).c_str(), L"wb");
  if (!f) {
    if (coInit) CoUninitialize();
    return false;
  }

  const size_t chunkSamples = sampleRate / 2;  // 0.5s per input sample
  const size_t frameBytes = static_cast<size_t>(inChannels) * 2;
  bool failed = false;
  bool wroteAny = false;
  size_t off = 0;
  for (;;) {
    const size_t n = std::min(chunkSamples, pcm.size() - off);
    if (n > 0) {
      Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
      if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(n * frameBytes), &buf))) {
        failed = true;
        break;
      }
      BYTE* ptr = nullptr;
      DWORD cur = 0, max = 0;
      if (FAILED(buf->Lock(&ptr, &max, &cur))) {
        failed = true;
        break;
      }
      const int16_t* src = pcm.data() + off;
      if (inChannels == channels) {
        std::memcpy(ptr, src, n * frameBytes);
      } else if (inChannels == 2 && channels == 1) {
        auto* dst = reinterpret_cast<int16_t*>(ptr);
        for (size_t i = 0; i < n; ++i) {
          dst[2 * i] = src[i];
          dst[2 * i + 1] = src[i];
        }
      } else if (inChannels == 1 && channels == 2) {
        auto* dst = reinterpret_cast<int16_t*>(ptr);
        for (size_t i = 0; i < n; ++i) {
          dst[i] = static_cast<int16_t>(
              (static_cast<int>(src[2 * i]) + static_cast<int>(src[2 * i + 1])) / 2);
        }
      } else {
        buf->Unlock();
        failed = true;
        break;
      }
      buf->SetCurrentLength(static_cast<DWORD>(n * frameBytes));
      buf->Unlock();

      Microsoft::WRL::ComPtr<IMFSample> sample;
      MFCreateSample(&sample);
      sample->AddBuffer(buf.Get());
      const HRESULT pi = enc->ProcessInput(0, sample.Get(), 0);
      if (FAILED(pi)) {
        // MF_E_NOTACCEPTING usually means we must drain first; loop again.
        failed = true;
        break;
      }
    }
    if (!drainOutput(enc.Get(), f, &wroteAny)) {
      failed = true;
      break;
    }
    if (n == 0) break;  // all input consumed
    off += n;
  }

  // Drain the encoder tail.
  if (!failed) {
    enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    for (;;) {
      MFT_OUTPUT_DATA_BUFFER odb{};
      Microsoft::WRL::ComPtr<IMFSample> outS;
      Microsoft::WRL::ComPtr<IMFMediaBuffer> ob;
      if (FAILED(MFCreateSample(&outS))) { failed = true; break; }
      if (FAILED(MFCreateMemoryBuffer(16384, &ob))) { failed = true; break; }
      outS->AddBuffer(ob.Get());
      odb.pSample = outS.Get();
      const HRESULT hd = enc->ProcessOutput(0, 1, &odb, nullptr);
      if (hd == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (FAILED(hd)) break;
      IMFMediaBuffer* mb = nullptr;
      if (odb.pSample) odb.pSample->GetBufferByIndex(0, &mb);
      if (mb) {
        BYTE* ptr = nullptr;
        DWORD cur = 0, max = 0;
        if (SUCCEEDED(mb->Lock(&ptr, &max, &cur)) && cur > 0) {
          std::fwrite(ptr, 1, cur, f);
          wroteAny = true;
          mb->Unlock();
        }
      }
    }
  }

  std::fclose(f);
  if (coInit) CoUninitialize();
  if (failed || !wroteAny) {
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    return false;
  }
  std::error_code ec;
  return std::filesystem::file_size(outPath, ec) > 0;
}

} // namespace aoi
