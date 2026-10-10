# MirrorCapture

Self-written native plugin for the JARVIS VR hand panel.

## Purpose

`MirrorCapture.dll` reads back the OpenVR compositor mirror texture (D3D11) and
writes it to a 32-bit BGRA BMP file on disk. It is the screenshot backbone of
the VR app: the mirror images feed the Aoi agent's screenshots and the
environment-awareness (1 fps frame capture) pipeline.

## API

```c
// Writes the mirror texture referenced by srv into outBmpPath (32-bit BGRA BMP).
// Returns 0 on success, negative error code on failure.
int MirrorShot(void* pDevice, void* pCtx, void* pSrv, const char* outBmpPath);
```

P/Invoke declaration in `unity-client/Assets/Aoi/Scripts/Core/AoiOrchestrator.cs`:

```csharp
[DllImport("MirrorCapture.dll")]
private static extern int MirrorShot(IntPtr device, IntPtr ctx, IntPtr srv, string outBmpPath);
```

## Build

```
build.cmd
```

Requires Visual Studio with MSVC x64 tools. Output lands at
`unity-client/Assets/Aoi/Plugins/x86_64/MirrorCapture.dll`, which Unity
copies into the build automatically.

## Provenance

The deployed DLL is byte-identical to the build produced from this source on
2026-08-01 (MSVC, `/O2 /MT`, linked against `d3d11.lib`). It exports a single
function `MirrorShot` and imports only standard Windows/CRT DLLs; no
third-party code is bundled.
