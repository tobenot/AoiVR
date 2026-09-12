cmake_minimum_required(VERSION 3.20)
project(aoi-agent-cpp VERSION 0.1.0 LANGUAGES CXX)

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
  src/tts.cpp
  src/sherpa_asr.cpp
  src/builtin_tools.cpp
  src/system_control.cpp
  src/llm_client.cpp
  src/remote_asr.cpp
  src/agent_config.cpp
  src/speech_segmenter.cpp
  src/interpreter.cpp
  src/environment_awareness.cpp
  src/agent.cpp
)

target_include_directories(aoi-agent PRIVATE
  src
  third_party
  third_party/nlohmann
  third_party/stb
  third_party/base64
  third_party/miniaudio
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
)

target_link_libraries(aoi-agent PRIVATE
  CURL::libcurl
  ws2_32
  winmm
  ole32
  oleaut32
  advapi32
  user32
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
  src/tts.cpp
  src/sherpa_asr.cpp
  src/builtin_tools.cpp
  src/system_control.cpp
  src/llm_client.cpp
  src/remote_asr.cpp
  src/agent_config.cpp
  src/speech_segmenter.cpp
  src/interpreter.cpp
  src/environment_awareness.cpp
  src/agent.cpp
)
add_library(aoi-agent-dll SHARED
  ${AOI_AGENT_SOURCES}
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
)
target_compile_definitions(aoi-agent-dll PRIVATE
  NOMINMAX
  STB_IMAGE_IMPLEMENTATION
  STB_IMAGE_WRITE_IMPLEMENTATION
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
  user32
)
if(MSVC)
  target_compile_options(aoi-agent-dll PRIVATE /utf-8 /permissive-)
endif()
if(AOI_ENABLE_SHERPA)
  target_compile_definitions(aoi-agent-dll PRIVATE AOI_USE_SHERPA)
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
)
target_include_directories(aoi-smoke-tests PRIVATE src third_party third_party/nlohmann third_party/stb third_party/base64)
target_compile_definitions(aoi-smoke-tests PRIVATE NOMINMAX STB_IMAGE_IMPLEMENTATION STB_IMAGE_WRITE_IMPLEMENTATION)
if(MSVC)
  target_compile_options(aoi-smoke-tests PRIVATE /utf-8 /permissive-)
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








