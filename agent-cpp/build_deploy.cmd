@echo off
rem Build aoi_agent.dll (C++ agent) and deploy it to the Unity plugin folder.
rem Output: unity-client/Assets/Aoi/Plugins/x86_64/aoi_agent.dll
rem Requires Visual Studio (MSVC x64 C++ tools) + CMake 3.20+.
rem The DLL is self-contained (static CRT + static curl/Schannel); only
rem Windows system DLLs are imported, so Unity loads it directly.
setlocal

set SRC=%~dp0
set UNITY_PLUGINS=%~dp0..\unity-client\Assets\Aoi\Plugins\x86_64
set BUILD=%SRC%build

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VSROOT=%%i
if not defined VSROOT (
    echo ERROR: Visual Studio with MSVC C++ tools not found.
    exit /b 1
)

rem Find the bundled CMake inside VS.
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
if not exist "%CMAKE%" set CMAKE=cmake

rem Generate build-time artifacts (prompts) if missing.
if not exist "%SRC%src\prompts.gen.hpp" (
    python "%SRC%..\tools\gen_prompts.py" >nul 2>&1
)

if not exist "%BUILD%\CMakeCache.txt" (
    "%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 || goto :err
)
"%CMAKE%" --build "%BUILD%" --config Release --target aoi-agent-dll || goto :err

if not exist "%UNITY_PLUGINS%" mkdir "%UNITY_PLUGINS%"
copy /y "%BUILD%\Release\aoi_agent.dll" "%UNITY_PLUGINS%\aoi_agent.dll" >nul
if errorlevel 1 goto :err
echo Built + deployed: %UNITY_PLUGINS%\aoi_agent.dll
exit /b 0

:err
echo ERROR: build/deploy failed.
exit /b 1
