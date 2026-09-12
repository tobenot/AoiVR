#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace aoi {

// Audio payload in the wire shape expected by OpenAI-compatible input_audio
// content blocks. `data` is raw base64 without the data-URL prefix.
struct RemoteAsrAudio {
  std::string data;
  std::string format;  // "wav" or "mp3"
};

struct RemoteAsrParseResult {
  bool ok = false;
  // Empty text is a valid result (the provider heard silence/no speech).
  std::string text;
};

// Parse a data:audio/*;base64,... URL into the input_audio payload. Only the
// formats already supported by the native-audio path are accepted.
bool parseRemoteAsrAudioDataUrl(const std::string& dataUrl, RemoteAsrAudio& out);

// Build a non-streaming /chat/completions request for the remote ASR stage.
nlohmann::json buildRemoteAsrRequest(const std::string& model,
                                     const std::string& prompt,
                                     const RemoteAsrAudio& audio);

// Parse either a normal OpenAI-compatible JSON response or an SSE response into
// one transcript. Provider error objects are rejected (ok=false).
RemoteAsrParseResult parseRemoteAsrResponse(const std::string& body);

}  // namespace aoi
