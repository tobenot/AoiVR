#include "system_control.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <combaseapi.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace aoi {

namespace {

struct Session {
  std::string process;
  DWORD pid = 0;
  float vol = 0;
  bool muted = false;
};

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string processName(DWORD pid) {
  if (pid == 0) return "System";
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "?";
  char buf[256] = {0};
  DWORD sz = sizeof(buf);
  std::string name = "?";
  if (QueryFullProcessImageNameA(h, 0, buf, &sz)) {
    const char* p = strrchr(buf, '\\');
    name = p ? p + 1 : buf;
  }
  CloseHandle(h);
  return name;
}

std::string deviceName(IMMDevice* dev) {
  IPropertyStore* props = nullptr;
  std::string out = "?";
  if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal) {
      int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
      if (n > 0) {
        out.assign(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out.data(), n, nullptr, nullptr);
        if (!out.empty()) out.pop_back();  // drop the trailing NUL
      }
    }
    PropVariantClear(&v);
    props->Release();
  }
  return out;
}

// Visit every active render device and collect its sessions (or apply a
// transform). `filter` (optional): case-insensitive process name; when empty
// every session is visited. Returns S_OK if at least one session was touched.
template <typename Fn>
HRESULT forEachSession(const std::string& filter, Fn fn) {
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr)) return hr;

  IMMDeviceCollection* coll = nullptr;
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
  if (FAILED(hr)) { enumerator->Release(); return hr; }

  const std::string want = lower(filter);
  UINT count = 0;
  coll->GetCount(&count);
  bool touched = false;
  for (UINT i = 0; i < count; i++) {
    IMMDevice* dev = nullptr;
    if (FAILED(coll->Item(i, &dev))) continue;
    IAudioSessionManager2* mgr = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&mgr)))) {
      IAudioSessionEnumerator* sen = nullptr;
      if (SUCCEEDED(mgr->GetSessionEnumerator(&sen))) {
        int n = 0;
        sen->GetCount(&n);
        for (int k = 0; k < n; k++) {
          IAudioSessionControl* ctl = nullptr;
          if (FAILED(sen->GetSession(k, &ctl))) continue;
          IAudioSessionControl2* ctl2 = nullptr;
          ISimpleAudioVolume* vol = nullptr;
          if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                            reinterpret_cast<void**>(&ctl2))) &&
              SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                            reinterpret_cast<void**>(&vol)))) {
            DWORD pid = 0;
            ctl2->GetProcessId(&pid);
            Session s;
            s.process = processName(pid);
            s.pid = pid;
            vol->GetMasterVolume(&s.vol);
            BOOL mute = FALSE;
            vol->GetMute(&mute);
            s.muted = mute != FALSE;
            // Exact, case-sensitive match against the process name returned
            // by get_volume — the caller already has the precise name.
            const bool match = want.empty() || s.process == filter;
            if (match && fn(dev, s, vol)) touched = true;
            vol->Release();
            ctl2->Release();
          }
          ctl->Release();
        }
        sen->Release();
      }
      mgr->Release();
    }
    dev->Release();
  }
  coll->Release();
  enumerator->Release();
  return touched ? S_OK : S_FALSE;
}

std::string hrMsg(const char* op, HRESULT hr) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", op, static_cast<unsigned long>(hr));
  return buf;
}

} // namespace

std::string setSessionVolumes(const std::string& process, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  const float level = static_cast<float>(percent) / 100.0f;

  const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ownInit = SUCCEEDED(hrInit);
  const HRESULT hr = forEachSession(
      process, [level](IMMDevice*, Session&, ISimpleAudioVolume* vol) {
        return SUCCEEDED(vol->SetMasterVolume(level, nullptr));
      });
  if (ownInit) CoUninitialize();
  if (FAILED(hr)) return hrMsg("SetSessionVolume", hr);
  if (hr == S_FALSE) {
    return process.empty() ? "no audio sessions found"
                           : "no audio session found for \"" + process + "\"";
  }
  return "";
}

std::string setSessionsMute(const std::string& process, bool mute) {
  const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ownInit = SUCCEEDED(hrInit);
  const HRESULT hr = forEachSession(
      process, [mute](IMMDevice*, Session&, ISimpleAudioVolume* vol) {
        return SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr));
      });
  if (ownInit) CoUninitialize();
  if (FAILED(hr)) return hrMsg("SetSessionMute", hr);
  if (hr == S_FALSE) {
    return process.empty() ? "no audio sessions found"
                           : "no audio session found for \"" + process + "\"";
  }
  return "";
}

std::string getSessionVolumes(std::string& outDesc) {
  outDesc.clear();
  const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ownInit = SUCCEEDED(hrInit);

  std::string desc;
  std::string curDevice;
  const HRESULT hr = forEachSession(
      "", [&desc, &curDevice](IMMDevice* dev, Session& s, ISimpleAudioVolume*) {
        std::string devName = deviceName(dev);
        if (devName != curDevice) {
          curDevice = devName;
          desc += (desc.empty() ? "" : "; ") + devName + ": ";
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "%s=%d%%%s", s.process.c_str(),
                 static_cast<int>(s.vol * 100.0f + 0.5f),
                 s.muted ? "(muted)" : "");
        desc += buf;
        desc += ", ";
        return true;
      });
  if (ownInit) CoUninitialize();

  if (FAILED(hr)) return hrMsg("GetSessionVolumes", hr);
  if (hr == S_FALSE) { outDesc = "no audio sessions found"; return ""; }
  // Trim trailing ", ".
  if (desc.size() >= 2) desc.resize(desc.size() - 2);
  outDesc = desc;
  return "";
}

} // namespace aoi
