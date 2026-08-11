param(
  [string]$OutDir = "D:\workplace\VR-AGENT\Aoi-Release",
  [switch]$SkipUnityBuild
)
$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot
$unity  = Join-Path $root "unity-client"
$build  = Join-Path $unity "Build"
$unityExe = "C:\Program Files\Unity 6000.5.4f1\Editor\Unity.exe"
$tmp = Join-Path $env:TEMP "opencode"

Write-Host "==> 1/4 Unity build (IL2CPP)"
if (-not $SkipUnityBuild) {
  # IL2CPP: MUST clear the Build dir first, else stale Mono output conflicts.
  if (Test-Path $build) {
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
  }
  & $unityExe -batchmode -nographics -quit -projectPath $unity -executeMethod BuildScript.Build -logFile (Join-Path $tmp "aoi_package_build.log")
  if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) { }
}

Write-Host "==> 2/4 verify IL2CPP layout"
$gameAssembly = Join-Path $build "GameAssembly.dll"
# Unity may still be flushing large IL2CPP outputs (GameAssembly.dll is tens of
# MB) to disk when its process exits; poll generously before giving up.
for ($i = 0; $i -lt 120 -and -not (Test-Path $gameAssembly); $i++) {
  Start-Sleep -Milliseconds 500
}
if (-not (Test-Path $gameAssembly)) {
  throw "GameAssembly.dll not found in Build root. Expected IL2CPP backend; check scriptingBackend = IL2CPP in ProjectSettings."
}
if (Test-Path (Join-Path $build "AoiVR_Data\Managed\Assembly-CSharp.dll")) {
  throw "Assembly-CSharp.dll present under AoiVR_Data\Managed - this is a Mono build, not IL2CPP. Rebuild with IL2CPP."
}

Write-Host "==> 3/4 assemble $OutDir"
if (Test-Path $OutDir) {
  for ($attempt = 0; $attempt -lt 3; $attempt++) {
    try { Remove-Item $OutDir -Recurse -Force -ErrorAction Stop; break }
    catch { Start-Sleep -Seconds 2 }
  }
  if (Test-Path $OutDir) { throw "Could not remove $OutDir (files locked?) - close any process using the package and retry" }
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# IL2CPP layout: the entire Build root must ship (GameAssembly.dll, UnityPlayer.dll,
# baselib, dstorage, D3D12/, ...). Skip only the backup/debug folders the names
# themselves say not to ship, PLUS the real .env (contains API keys — the shipped
# build reads it for local testing, but a release must never carry real keys).
Get-ChildItem $build | Where-Object {
  $_.Name -notmatch "BackUpThisFolder_ButDontShipItWithYourGame" -and
  $_.Name -notmatch "_BurstDebugInformation_DoNotShip" -and
  $_.Name -ne ".env" -and
  $_.Name -ne "aoi_debug.txt" -and
  $_.Name -ne "rt_capture.png"
} | ForEach-Object {
  Copy-Item $_.FullName $OutDir -Recurse -Force
}

# aoi_agent.dll (embedded C++ agent) is inside Assets/Aoi/Plugins, so Unity
# already baked it into the build. Verify it survived.
$agentDll = Join-Path $OutDir "AoiVR_Data\Plugins\x86_64\aoi_agent.dll"
if (-not (Test-Path $agentDll)) {
  Write-Host "WARNING: aoi_agent.dll not found under AoiVR_Data\Plugins - native agent may be missing from the package!"
}

# config: never ship real aoi_config.json (contains API keys); ship the
# template only. The agent reads aoi_config.json from its workdir (exe dir).
$configExample = Join-Path $root "agent-cpp\aoi_config.json.example"
if (Test-Path $configExample) { Copy-Item $configExample (Join-Path $OutDir "aoi_config.json.example") }

# Native sandbox binaries: the agent runs INSIDE aoi_agent.dll and resolves
# the helper/setup exes next to the PROCESS exe (AoiVR.exe) - i.e. the package
# root. Without them every tool call fails with "(sandbox: helper not found)".
foreach ($bin in @("aoi-sandbox-helper.exe", "aoi-sandbox-setup.exe")) {
  $srcBin = Join-Path $root "agent-cpp\build\Release\$bin"
  if (Test-Path $srcBin) {
    Copy-Item $srcBin (Join-Path $OutDir $bin)
    Write-Host "  + sandbox bin: $bin"
  } else {
    Write-Host "WARNING: $bin not found at $srcBin - sandbox tools will fail at runtime"
  }
}

# licenses / notices
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $OutDir
# Verbatim official license texts (referenced by the NOTICES index).
Copy-Item (Join-Path $root "licenses") (Join-Path $OutDir "licenses") -Recurse -Force

# VRChat integration skill docs (agent runtime loads these via the read tool;
# the build normally carries them too, but copy from source so the package is
# complete even if the Unity build step was skipped).
$skillSrc = Join-Path $root "docs\vrchat-assistant"
$skillDst = Join-Path $OutDir "docs\vrchat-assistant"
if (Test-Path $skillSrc) {
  if (Test-Path $skillDst) { Remove-Item $skillDst -Recurse -Force }
  Copy-Item $skillSrc $skillDst -Recurse -Force
}

Write-Host "==> 4/4 launch.bat"
$launch = @'
@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
rem Point awareness frame dir at the build's context/frames.
if not exist "context\frames" mkdir "context\frames" >nul 2>&1
set "AOI_FRAMES_DIR=%~dp0context\frames"

rem --- First run: create aoi_config.json interactively if missing (or keys empty). ---
set "NEED_CFG=0"
if not exist "aoi_config.json" set "NEED_CFG=1"
if exist "aoi_config.json" (
  findstr /b "\"apiKey\":" "aoi_config.json" >nul 2>&1 || set "NEED_CFG=1"
  findstr "\"apiKey\": \"\"" "aoi_config.json" >nul 2>&1 && set "NEED_CFG=1"
)
if "!NEED_CFG!"=="1" (
  echo.
  echo  First run: create aoi_config.json with your API keys.
  echo  Copy aoi_config.json.example and fill in:
  echo    llm.apiKey   - OpenCode: https://opencode.ai/auth
  echo    tts.apiKey   - MiMo:     https://platform.xiaomimimo.com/
  echo.
  if not exist "aoi_config.json" (
    copy /y "aoi_config.json.example" "aoi_config.json" >nul
    echo  Created aoi_config.json from the template. Edit it, then run again.
    echo.
    pause
    exit /b 1
  )
)

echo Starting AoiVR (SteamVR overlay)...
start "" "%~dp0AoiVR.exe"
endlocal
'@
[System.IO.File]::WriteAllText((Join-Path $OutDir "launch.bat"), $launch, [System.Text.Encoding]::Default)

$readme = @"
Aoi - VR 应用
=============

运行方法：
  1. 安装 SteamVR 运行环境。
  2. 复制 aoi_config.json.example 为 aoi_config.json 并填入密钥：
        llm.apiKey - 注册地址 https://opencode.ai/auth
        tts.apiKey - 注册地址 https://platform.xiaomimimo.com/
  3. 双击 launch.bat 启动。

第三方组件与开源许可：
  - 本项目使用以下第三方开源组件（均为宽松许可，可闭源分发）：
    OpenVR (BSD-3-Clause)、nlohmann/json (MIT)、libcurl (curl license)、
    miniaudio (Public Domain / MIT-0)、stb_image (Public Domain / MIT)、
    base64 (MIT)、Liberation Sans / Noto Sans CJK / JetBrains Mono (OFL-1.1)
  - 完整清单与官方许可文本见 THIRD_PARTY_NOTICES.md 及 licenses/ 目录。
"@
[System.IO.File]::WriteAllText((Join-Path $OutDir "README.txt"), $readme, [System.Text.Encoding]::UTF8)

Write-Host "Done. Package at $OutDir"
