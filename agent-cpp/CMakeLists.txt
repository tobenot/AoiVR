cmake_minimum_required(VERSION 3.20)
project(aoi-agent-cpp VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Release flags: /O2 optimize, /EHsc exceptions, /GS stack cookies.
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /EHsc /GS")

add_executable(aoi-agent
  src/main.cpp
  src/types.cpp
  src/protocol.cpp
  src/base64.cpp
  src/http_client.cpp
  src/audio_utils.cpp
  src/agent_utils.cpp
  src/image_utils.cpp
  src/mic.cpp
  src/player.cpp
  src/speaker.cpp
  src/miniaudio_impl.cpp
  src/mp3_encoder.cpp
  src/tts.cpp
  src/sherpa_asr.cpp
  src/builtin_tools.cpp
  src/sqlite_tools.cpp
  src/convert_tools.cpp
  src/fetch_tools.cpp
  src/system_control.cpp
  src/llm_client.cpp
  src/remote_asr.cpp
  src/agent_config.cpp
  src/speech_segmenter.cpp
  src/interpreter.cpp
  src/environment_awareness.cpp
  src/hook_scheduler.cpp
  src/hook_tools.cpp
  src/registry.cpp
  src/skills.cpp
  src/windows_sandbox.cpp
  src/agent.cpp
  third_party/sqlite/sqlite3.c
)

target_include_directories(aoi-agent PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/stb
  third_party/base64
  third_party/miniaudio
  third_party/sqlite
)

# Static CRT for ALL targets (curl included) so aoi_agent.dll is self-contained
# inside Unity (no vcruntime dlls to ship).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

# ---- libcurl (Schannel TLS, no OpenSSL) ----
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_TESTS OFF CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/curl)

# stb implementations (STB_IMAGE_RESIZE_IMPLEMENTATION is defined in
# image_utils.cpp so it only compiles there)
target_compile_definitions(aoi-agent PRIVATE
  NOMINMAX
  STB_IMAGE_IMPLEMENTATION
  STB_IMAGE_WRITE_IMPLEMENTATION
  SQLITE_THREADSAFE=1
)

target_link_libraries(aoi-agent PRIVATE
  CURL::libcurl
  ws2_32
  winmm
  ole32
  oleaut32
  advapi32
  crypt32
  user32
  mfplat
  mfreadwrite
  mf
  mfuuid
)

# Optional sherpa-onnx (local ASR/VAD). Enabled when the library is provided.
option(AOI_ENABLE_SHERPA "Enable sherpa-onnx ASR/VAD" OFF)
if(AOI_ENABLE_SHERPA)
  target_compile_definitions(aoi-agent PRIVATE AOI_USE_SHERPA)
endif()

if(MSVC)
  target_compile_options(aoi-agent PRIVATE /utf-8 /permissive-)
endif()

# ---- AOI agent as a shared library for Unity P/Invoke ----
# The agent runs on its own background thread (see aoi_agent_api.cpp), so
# Unity's main thread / render loop is never blocked.
set(AOI_AGENT_SOURCES
  src/types.cpp
  src/protocol.cpp
  src/base64.cpp
  src/http_client.cpp
  src/audio_utils.cpp
  src/agent_utils.cpp
  src/image_utils.cpp
  src/mic.cpp
  src/player.cpp
  src/speaker.cpp
  src/miniaudio_impl.cpp
  src/mp3_encoder.cpp
  src/tts.cpp
  src/sherpa_asr.cpp
  src/builtin_tools.cpp
  src/sqlite_tools.cpp
  src/convert_tools.cpp
  src/fetch_tools.cpp
  src/system_control.cpp
  src/llm_client.cpp
  src/remote_asr.cpp
  src/agent_config.cpp
  src/speech_segmenter.cpp
  src/interpreter.cpp
  src/environment_awareness.cpp
  src/hook_scheduler.cpp
  src/hook_tools.cpp
  src/registry.cpp
  src/skills.cpp
  src/windows_sandbox.cpp
  src/agent.cpp
)
add_library(aoi-agent-dll SHARED
  ${AOI_AGENT_SOURCES}
  third_party/sqlite/sqlite3.c
  src/aoi_agent_api.cpp
)
set_target_properties(aoi-agent-dll PROPERTIES OUTPUT_NAME "aoi_agent")
target_include_directories(aoi-agent-dll PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/stb
  third_party/base64
  third_party/miniaudio
  third_party/sqlite
)
target_compile_definitions(aoi-agent-dll PRIVATE
  NOMINMAX
  STB_IMAGE_IMPLEMENTATION
  STB_IMAGE_WRITE_IMPLEMENTATION
  SQLITE_THREADSAFE=1
  AOI_BUILD_DLL
)
# Static CRT so the DLL is self-contained inside Unity (no vcruntime dlls to ship).
set_target_properties(aoi-agent-dll PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(aoi-agent-dll PRIVATE
  CURL::libcurl
  ws2_32
  winmm
  ole32
  oleaut32
  advapi32
  crypt32
  user32
  mfplat
  mfreadwrite
  mf
  mfuuid
)
if(MSVC)
  target_compile_options(aoi-agent-dll PRIVATE /utf-8 /permissive-)
endif()
if(AOI_ENABLE_SHERPA)
  target_compile_definitions(aoi-agent-dll PRIVATE AOI_USE_SHERPA)
endif()

# ---- aoi-sandbox-helper: runs INSIDE the restricted-token sandbox and is the
# only place model-controlled writes execute (aligned with Codex exec-server).
add_executable(aoi-sandbox-helper
  src/helper/main.cpp
  src/agent_utils.cpp
  third_party/sqlite/sqlite3.c
)
target_include_directories(aoi-sandbox-helper PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/base64
  third_party/sqlite
)
target_compile_definitions(aoi-sandbox-helper PRIVATE
  NOMINMAX
  SQLITE_THREADSAFE=1
)
target_link_libraries(aoi-sandbox-helper PRIVATE
  advapi32
)
if(MSVC)
  target_compile_options(aoi-sandbox-helper PRIVATE /utf-8 /permissive-)
endif()

# ---- aoi-sandbox-setup: one-time elevated configuration (creates the
# sandbox user/group, ACLs, DPAPI-encrypted credentials). Launched with UAC
# by the agent when the sandbox has not been provisioned yet.
add_executable(aoi-sandbox-setup
  src/setup_main/main.cpp
)
target_include_directories(aoi-sandbox-setup PRIVATE
  src
  third_party
  third_party/nlohmann
)
target_compile_definitions(aoi-sandbox-setup PRIVATE NOMINMAX)
target_link_libraries(aoi-sandbox-setup PRIVATE
  advapi32
  crypt32
  netapi32
  userenv
)
if(MSVC)
  target_compile_options(aoi-sandbox-setup PRIVATE /utf-8 /permissive-)
endif()

# ---- sandbox_test: standalone sandbox chain test (dev tool) ----
add_executable(aoi-sandbox-test
  src/sandbox_test_main.cpp
  src/windows_sandbox.cpp
)
target_include_directories(aoi-sandbox-test PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/base64
)
target_compile_definitions(aoi-sandbox-test PRIVATE NOMINMAX)
target_link_libraries(aoi-sandbox-test PRIVATE
  advapi32
  crypt32
  user32
)
if(MSVC)
  target_compile_options(aoi-sandbox-test PRIVATE /utf-8 /permissive-)
endif()

# ---- aoi-hook-test: standalone hook scheduler test (dev tool) ----
add_executable(aoi-hook-test
  src/hook_test_main.cpp
  src/hook_scheduler.cpp
  src/windows_sandbox.cpp
  third_party/sqlite/sqlite3.c
)
target_include_directories(aoi-hook-test PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/base64
  third_party/sqlite
)
target_compile_definitions(aoi-hook-test PRIVATE NOMINMAX SQLITE_THREADSAFE=1)
target_link_libraries(aoi-hook-test PRIVATE advapi32 crypt32 user32)
if(MSVC)
  target_compile_options(aoi-hook-test PRIVATE /utf-8 /permissive-)
endif()

# ---- smoke tests (pure modules only, no hardware) ----
add_executable(aoi-smoke-tests
  tests/smoke_test.cpp
  src/types.cpp
  src/protocol.cpp
  src/base64.cpp
  src/audio_utils.cpp
  src/agent_utils.cpp
  src/image_utils.cpp
  src/miniaudio_impl.cpp
)
target_include_directories(aoi-smoke-tests PRIVATE src third_party third_party/nlohmann third_party/stb third_party/base64 third_party/miniaudio)
target_compile_definitions(aoi-smoke-tests PRIVATE NOMINMAX STB_IMAGE_IMPLEMENTATION STB_IMAGE_WRITE_IMPLEMENTATION)
target_link_libraries(aoi-smoke-tests PRIVATE winmm ole32)
if(MSVC)
  target_compile_options(aoi-smoke-tests PRIVATE /utf-8 /permissive-)
endif()

add_executable(aoi-registry-tests
  tests/registry_test.cpp
  src/registry.cpp
)
target_include_directories(aoi-registry-tests PRIVATE src third_party third_party/nlohmann third_party/base64)
target_compile_definitions(aoi-registry-tests PRIVATE NOMINMAX)
if(MSVC)
  target_compile_options(aoi-registry-tests PRIVATE /utf-8 /permissive-)
endif()

add_executable(aoi-skills-tests
  tests/skills_test.cpp
  src/skills.cpp
)
target_include_directories(aoi-skills-tests PRIVATE src third_party third_party/nlohmann third_party/base64)
target_compile_definitions(aoi-skills-tests PRIVATE NOMINMAX)
if(MSVC)
  target_compile_options(aoi-skills-tests PRIVATE /utf-8 /permissive-)
endif()

add_executable(aoi-sse-tests
  tests/sse_test.cpp
  src/base64.cpp
  src/http_client.cpp
  src/tts.cpp
)
target_include_directories(aoi-sse-tests PRIVATE src third_party third_party/nlohmann third_party/base64)
if(MSVC)
  target_compile_options(aoi-sse-tests PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(aoi-sse-tests PRIVATE CURL::libcurl ws2_32)

add_executable(aoi-llm-tests
  tests/llm_test.cpp
  src/base64.cpp
  src/http_client.cpp
  src/llm_client.cpp
  src/remote_asr.cpp
)
target_include_directories(aoi-llm-tests PRIVATE src third_party third_party/nlohmann third_party/base64)
if(MSVC)
  target_compile_options(aoi-llm-tests PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(aoi-llm-tests PRIVATE CURL::libcurl ws2_32)

add_executable(aoi-remote-asr-tests
  tests/remote_asr_test.cpp
  src/remote_asr.cpp
)
target_include_directories(aoi-remote-asr-tests PRIVATE src third_party third_party/nlohmann)
if(MSVC)
  target_compile_options(aoi-remote-asr-tests PRIVATE /utf-8 /permissive-)
endif()

add_executable(aoi-config-tests
  tests/config_test.cpp
  src/agent_config.cpp
)
target_include_directories(aoi-config-tests PRIVATE src third_party third_party/nlohmann)
if(MSVC)
  target_compile_options(aoi-config-tests PRIVATE /utf-8 /permissive-)
endif()
# Dev tool: prompt-cache hit sequence measurement (long-text turns).
add_executable(aoi-cache-lab
  tests/cache_lab.cpp
  src/base64.cpp
  src/http_client.cpp
  src/llm_client.cpp
  src/remote_asr.cpp
)
target_include_directories(aoi-cache-lab PRIVATE src third_party third_party/nlohmann third_party/base64)
if(MSVC)
  target_compile_options(aoi-cache-lab PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(aoi-cache-lab PRIVATE CURL::libcurl ws2_32)

# fetch tool validation tests + live-check mode.
add_executable(aoi-fetch-test
  tests/fetch_test.cpp
  src/fetch_tools.cpp
  src/agent_utils.cpp
  src/http_client.cpp
)
target_include_directories(aoi-fetch-test PRIVATE src third_party third_party/nlohmann)
if(MSVC)
  target_compile_options(aoi-fetch-test PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(aoi-fetch-test PRIVATE CURL::libcurl ws2_32)

# Dev tool: generate a real speech wav via MiMo TTS for cache experiments.
add_executable(aoi-tts-gen
  tests/tts_gen.cpp
  src/base64.cpp
  src/http_client.cpp
  src/tts.cpp
)
target_include_directories(aoi-tts-gen PRIVATE src third_party third_party/nlohmann third_party/base64)
if(MSVC)
  target_compile_options(aoi-tts-gen PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(aoi-tts-gen PRIVATE CURL::libcurl ws2_32)

# Dev tool: WMF mp3 encoder feasibility test.
add_executable(aoi-mp3-test
  tests/mp3_test.cpp
  src/mp3_encoder.cpp
)
target_include_directories(aoi-mp3-test PRIVATE src third_party)
target_compile_definitions(aoi-mp3-test PRIVATE NOMINMAX)
if(MSVC)
  target_compile_options(aoi-mp3-test PRIVATE /utf-8 /permissive-)
endif()
target_link_libraries(aoi-mp3-test PRIVATE mfplat mfreadwrite mf mfuuid)

add_executable(aoi-dll-tests
  tests/dll_test.cpp
)
if(MSVC)
  target_compile_options(aoi-dll-tests PRIVATE /utf-8 /permissive-)
endif()
# Place next to the DLL so LoadLibrary("aoi_agent.dll") resolves.
add_custom_command(TARGET aoi-dll-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:aoi-agent-dll> $<TARGET_FILE_DIR:aoi-dll-tests>)
add_dependencies(aoi-dll-tests aoi-agent-dll)

add_executable(aoi-transport-tests
  tests/transport_test.cpp
)
if(MSVC)
  target_compile_options(aoi-transport-tests PRIVATE /utf-8 /permissive-)
endif()
add_custom_command(TARGET aoi-transport-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:aoi-agent-dll> $<TARGET_FILE_DIR:aoi-transport-tests>)
add_dependencies(aoi-transport-tests aoi-agent-dll)

add_executable(aoi-stability-tests
  tests/stability_test.cpp
)
if(MSVC)
  target_compile_options(aoi-stability-tests PRIVATE /utf-8 /permissive-)
endif()
add_custom_command(TARGET aoi-stability-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:aoi-agent-dll> $<TARGET_FILE_DIR:aoi-stability-tests>)
add_dependencies(aoi-stability-tests aoi-agent-dll)








