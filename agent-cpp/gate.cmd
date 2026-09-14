@echo off
rem Quality gate: build everything, run ALL test suites, fail on any failure.
rem Exit code 0 = gate passed (code is accepted); nonzero = rejected.
setlocal enabledelayedexpansion

set ROOT=%~dp0
rem Drop the trailing backslash: a quoted value ending in "\" (e.g. "%ROOT%")
rem confuses cmd's quote pairing and corrupts every later argument (cmake
rem then sees "-G Visual" instead of "-G Visual Studio 18 2026").
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set BUILD=%ROOT%\build
set RELEASE=%BUILD%\Release
set FAILED=0

rem --- 1. Configure (if needed) + build all ---
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VSROOT=%%i
if not defined VSROOT (
    echo [GATE] ERROR: MSVC not found
    exit /b 1
)
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
if not exist "%CMAKE%" set CMAKE=cmake

if not exist "%BUILD%\CMakeCache.txt" (
    "%CMAKE%" -S "%ROOT%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 ( echo [GATE] configure failed & exit /b 1 )
)
"%CMAKE%" --build "%BUILD%" --config Release
if errorlevel 1 ( echo [GATE] build failed & exit /b 1 )

rem --- 2. Run every test suite; any nonzero/abnormal => gate failed ---
set TESTS=aoi-smoke-tests aoi-registry-tests aoi-skills-tests aoi-sse-tests aoi-llm-tests aoi-remote-asr-tests aoi-config-tests aoi-transport-tests aoi-dll-tests aoi-stability-tests aoi-fetch-test
for %%T in (%TESTS%) do (
    echo.
    echo ===== [GATE] %%T =====
    "%RELEASE%\%%T.exe"
    if errorlevel 1 (
        echo [GATE] FAILED: %%T
        set FAILED=1
    ) else (
        echo [GATE] PASSED: %%T
    )
)

if "%FAILED%"=="1" (
    echo.
    echo [GATE] RESULT: FAILED - code rejected
    exit /b 1
)
echo.
echo [GATE] RESULT: ALL PASSED - code accepted
exit /b 0
