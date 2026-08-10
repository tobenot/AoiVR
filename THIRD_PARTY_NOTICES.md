# Third-Party Notices

This project (open source, MIT — see LICENSE) incorporates the following
third-party components. Each section lists the component, the official
source URL, and the verbatim license file location.

License texts are NOT reproduced here. The verbatim texts live in the files
listed below; each was downloaded directly from the official source URL given.
No license text in this repository is hand-written.

Everything here is permissive and compatible with the MIT license. No
GPL/AGPL/LGPL/copyleft component is used at runtime.

---

## 1. OpenVR (openvr_api) — Valve Corporation

- License: BSD-3-Clause
- Official source: https://github.com/ValveSoftware/openvr
- Official license URL: https://raw.githubusercontent.com/ValveSoftware/openvr/master/LICENSE
- Local license file: `licenses/openvr.txt`
- Local code files:
  - unity-client/Assets/Aoi/Plugins/x86_64/openvr_api.dll
  - unity-client/Assets/Aoi/Scripts/Core/OpenVR/openvr_api.cs
- Used in: AoiOrchestrator.cs (overlay, compositor mirror, controller input).

---

## 2. Liberation Sans — Red Hat / Liberation Fonts project

- License: SIL Open Font License 1.1
- Official source: https://github.com/liberationfonts/liberation-fonts
- Official license URL: https://raw.githubusercontent.com/liberationfonts/liberation-fonts/master/LICENSE
- Local license file: `licenses/liberation.txt`
- Local code file: unity-client/Assets/TextMesh Pro/Fonts/LiberationSans.ttf
- Used in: UI body text.

---

## 3. Noto Sans CJK SC (NotoSansCJKsc-Regular.otf) — Google / Adobe

- License: SIL Open Font License 1.1
- Official source: https://github.com/googlefonts/noto-cjk
- Official license URL: https://raw.githubusercontent.com/googlefonts/noto-cjk/main/Sans/LICENSE
- Local license file: `licenses/noto.txt`
- Local code file: unity-client/Assets/Aoi/Resources/NotoSansCJKsc-Regular.otf
- Used in: Chinese fallback text.

---

## 4. JetBrains Mono (JetBrainsMono-Regular.ttf) — JetBrains s.r.o.

- License: SIL Open Font License 1.1
- Official source: https://github.com/JetBrains/JetBrainsMono
- Official license URL: https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/OFL.txt
- Local license file: `licenses/jetbrains.txt`
- Local code file: unity-client/Assets/Aoi/Resources/JetBrainsMono-Regular.ttf
- Used in: monospace text (logo, status, hints).

---

## 5. nlohmann/json — Niels Lohmann

- License: MIT
- Official source: https://github.com/nlohmann/json
- Version: v3.11.3 (single-header)
- Official license URL: https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT
- Local license file: `licenses/nlohmann.txt`
- Local code file: agent-cpp/third_party/nlohmann/json.hpp
- Used in: agent-cpp LLM/TTS/SSE JSON handling.

---

## 6. stb_image / stb_image_write / stb_image_resize2 — Sean Barrett et al.

- License: MIT OR Public Domain (Unlicense) — dual
- Official source: https://github.com/nothings/stb
- Official license URL: https://raw.githubusercontent.com/nothings/stb/master/LICENSE
- Local license file: `licenses/stb_license.txt`
- Local code files:
  - agent-cpp/third_party/stb/stb_image.h (v2.30)
  - agent-cpp/third_party/stb/stb_image_write.h (v1.16)
  - agent-cpp/third_party/stb/stb_image_resize2.h (v2.18)
- Used in: agent-cpp image decode/resize/encode (screenshots).
- Note: `stb_image.h` embeds its own zlib inflate implementation (PNG decoding,
  `stbi_zlib_decode_*`). This is NOT the external zlib library — no zlib is
  linked (curl built with HAVE_LIBZ undef; the DLL imports only Windows system
  libraries). The embedded decoder is part of stb, same Public Domain / MIT
  dual license, so no separate zlib notice is required.

---

## 7. base64 (Google-style base64) — Tomas Kislan / Adam Rudd

- License: MIT
- Official source: https://github.com/tkislan/base64
- Official license URL: https://raw.githubusercontent.com/tkislan/base64/master/LICENSE
- Local license file: `licenses/base64.txt`
- Local code file: agent-cpp/third_party/base64/base64.h
- Used in: agent-cpp base64 encode/decode.

---

## 8. libcurl — Daniel Stenberg / curl project

- License: curl License (MIT-like, ISC-style)
- Official source: https://github.com/curl/curl
- Version: 8.11.0 (vendored source, static, Schannel TLS)
- Official license URL: https://raw.githubusercontent.com/curl/curl/curl-8_11_0/COPYING
- Local license file: `licenses/curl.txt`
- Local code files: agent-cpp/third_party/curl/
- Used in: agent-cpp HTTPS/SSE client.

---

## 9. miniaudio — David Reid (mackron)

- License: Public Domain (Unlicense) OR MIT No Attribution (MIT-0)
- Official source: https://github.com/mackron/miniaudio
- Official license URL: https://raw.githubusercontent.com/mackron/miniaudio/master/LICENSE
- Local license file: `licenses/miniaudio.txt`
- Local code file: agent-cpp/third_party/miniaudio/miniaudio.h
- Used in: agent-cpp mic capture, playback, speaker loopback.
- Note: also embeds dr_wav/dr_mp3/dr_flac/stb_vorbis (same terms); shipped code
  only uses the WAV/PCM path.

---

## 10. sherpa-onnx (OPTIONAL, AOI_ENABLE_SHERPA=ON — not in this build)

- License: Apache-2.0
- Official source: https://github.com/k2-fsa/sherpa-onnx
- Official license URL: https://www.apache.org/licenses/LICENSE-2.0.txt
- Local license file: `licenses/apache2.txt`
- NOT enabled in the released binaries (AOI_ENABLE_SHERPA is OFF). If enabled
  in a future build, this notice must ship with it.

---

## 11. Unity Engine and Unity packages

Governed by Unity's license terms (Unity EULA / Unity Companion License), not
open-source in the copyleft sense. Components used: Unity Engine 6000.5.4f1,
com.unity.render-pipelines.universal (URP) 17.0.3 (lock 17.5.0),
com.unity.textmeshpro 3.0.9, com.unity.multiplayer.center 1.0.1, plus
transitive packages (burst, mathematics, collections, render-pipelines.core,
universal-config, shadergraph, ugui, searcher, mono-cecil, test-framework,
ext.nunit) and the standard com.unity.modules.* built-in modules.

---

## 12. Windows / Microsoft platform components (OS-provided)

- ole32 / winmm / dsound / avrt — miniaudio audio backends
- user32 / advapi32 — OS interop
- Windows CoreAudio COM (IAudioEndpointVolume) — system volume control
- dstorage.dll / dstoragecore.dll / D3D12Core.dll — DirectStorage / D3D12
  runtime shipped by Unity with the player.

---

## 13. SQLite — SQLite Consortium (D. Richard Hipp)

- License: Public Domain
- Official source: https://www.sqlite.org/
- Version: 3.53.4 (amalgamation, vendored: sqlite3.c / sqlite3.h / sqlite3ext.h)
- Official license URL: https://www.sqlite.org/copyright.html
- Local license file: `licenses/sqlite.txt`
- Local code files: agent-cpp/third_party/sqlite/
- Used in: agent-cpp sql_query tool (read-only access to the VRCX SQLite
  session store).

---

## Compliance summary

| Component | License | Official license URL | Verbatim file |
|---|---|---|---|
| OpenVR | BSD-3-Clause | https://raw.githubusercontent.com/ValveSoftware/openvr/master/LICENSE | `licenses/openvr.txt` |
| Liberation Sans | OFL-1.1 | https://raw.githubusercontent.com/liberationfonts/liberation-fonts/master/LICENSE | `licenses/liberation.txt` |
| Noto Sans CJK SC | OFL-1.1 | https://raw.githubusercontent.com/googlefonts/noto-cjk/main/Sans/LICENSE | `licenses/noto.txt` |
| JetBrains Mono | OFL-1.1 | https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/OFL.txt | `licenses/jetbrains.txt` |
| nlohmann/json | MIT | https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT | `licenses/nlohmann.txt` |
| stb | MIT / Public Domain | https://raw.githubusercontent.com/nothings/stb/master/LICENSE | `licenses/stb_license.txt` |
| base64 | MIT | https://raw.githubusercontent.com/tkislan/base64/master/LICENSE | `licenses/base64.txt` |
| libcurl | curl license | https://raw.githubusercontent.com/curl/curl/curl-8_11_0/COPYING | `licenses/curl.txt` |
| miniaudio | Public Domain / MIT-0 | https://raw.githubusercontent.com/mackron/miniaudio/master/LICENSE | `licenses/miniaudio.txt` |
| sherpa-onnx (optional) | Apache-2.0 | https://www.apache.org/licenses/LICENSE-2.0.txt | `licenses/apache2.txt` |
| SQLite | Public Domain | https://www.sqlite.org/copyright.html | `licenses/sqlite.txt` |
| Unity Engine + packages | Unity EULA | (Unity license terms) | — |

No GPL / AGPL / LGPL / MPL-copyleft component is used at runtime.
