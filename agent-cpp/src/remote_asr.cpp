#include "remote_asr.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace aoi {
namespace {

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string trimAscii(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool appendContentText(const nlohmann::json& content, std::string& out) {
  if (content.is_string()) {
    out += content.get<std::string>();
    return true;
  }
  if (!content.is_array()) return false;

  bool found = false;
  for (const auto& part : content) {
    if (!part.is_object()) continue;
    if (part.contains("text") && part["text"].is_string()) {
      out += part["text"].get<std::string>();
      found = true;
    } else if (part.contains("content") && part["content"].is_string()) {
      out += part["content"].get<std::string>();
      found = true;
    }
  }
  return found;
}

// Extract text from one complete OpenAI-compatible response or stream frame.
bool appendResponseText(const nlohmann::json& root, std::string& out) {
  bool found = false;

  if (root.is_object()) {
    if (root.contains("text") && root["text"].is_string()) {
      out += root["text"].get<std::string>();
      found = true;
    }
    if (root.contains("output_text") && root["output_text"].is_string()) {
      out += root["output_text"].get<std::string>();
      found = true;
    }

    if (root.contains("choices") && root["choices"].is_array()) {
      for (const auto& choice : root["choices"]) {
        if (!choice.is_object()) continue;
        const nlohmann::json* message = nullptr;
        if (choice.contains("message") && choice["message"].is_object()) {
          message = &choice["message"];
        } else if (choice.contains("delta") && choice["delta"].is_object()) {
          message = &choice["delta"];
        }
        if (!message) continue;
        if (message->contains("content")) {
          found = appendContentText((*message)["content"], out) || found;
        }
        if (message->contains("text") && (*message)["text"].is_string()) {
          out += (*message)["text"].get<std::string>();
          found = true;
        }
      }
    }

    // A few transcription-compatible gateways wrap the result as {data:{text}}
    // instead of returning the OpenAI chat shape.
    if (root.contains("data") && root["data"].is_object() &&
        root["data"].contains("text") && root["data"]["text"].is_string()) {
      out += root["data"]["text"].get<std::string>();
      found = true;
    }
  }
  return found;
}

}  // namespace

bool parseRemoteAsrAudioDataUrl(const std::string& dataUrl, RemoteAsrAudio& out) {
  if (dataUrl.rfind("data:", 0) != 0) return false;
  const size_t comma = dataUrl.find(',');
  if (comma == std::string::npos || comma <= 5 || comma + 1 >= dataUrl.size()) return false;

  const std::string metadata = lowerAscii(dataUrl.substr(5, comma - 5));
  if (metadata.find(";base64") == std::string::npos) return false;
  const size_t semicolon = metadata.find(';');
  const std::string mime = metadata.substr(0, semicolon);

  std::string format;
  if (mime == "audio/wav" || mime == "audio/wave" || mime == "audio/x-wav") {
    format = "wav";
  } else if (mime == "audio/mpeg" || mime == "audio/mp3") {
    format = "mp3";
  } else {
    return false;
  }

  out.data = dataUrl.substr(comma + 1);
  out.format = std::move(format);
  return !out.data.empty();
}

nlohmann::json buildRemoteAsrRequest(const std::string& model,
                                     const std::string& prompt,
                                     const RemoteAsrAudio& audio) {
  nlohmann::json content = nlohmann::json::array();
  content.push_back({{"type", "text"}, {"text", prompt}});
  content.push_back({{"type", "input_audio"},
                     {"input_audio", {{"data", audio.data},
                                      {"format", audio.format}}}});

  nlohmann::json messages = nlohmann::json::array();
  messages.push_back({{"role", "user"}, {"content", std::move(content)}});

  return nlohmann::json{{"model", model},
                        {"stream", false},
                        {"messages", std::move(messages)}};
}

RemoteAsrParseResult parseRemoteAsrResponse(const std::string& body) {
  RemoteAsrParseResult result;
  const std::string trimmed = trimAscii(body);
  if (trimmed.empty()) return result;

  // Some compatible endpoints ignore stream=false and still return SSE. Parse
  // every data frame and concatenate only its text deltas.
  if (trimmed.rfind("data:", 0) == 0 ||
      trimmed.find("\ndata:") != std::string::npos) {
    std::istringstream stream(body);
    std::string line;
    bool sawFrame = false;
    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) != 0) continue;
      std::string payload = trimAscii(line.substr(5));
      if (payload.empty() || payload == "[DONE]") continue;
      try {
        const auto frame = nlohmann::json::parse(payload);
        if (frame.is_object() && frame.contains("error")) return result;
        appendResponseText(frame, result.text);
        sawFrame = true;
      } catch (...) {
        return RemoteAsrParseResult{};
      }
    }
    if (sawFrame) {
      result.ok = true;
      result.text = trimAscii(result.text);
    }
    return result;
  }

  try {
    const auto json = nlohmann::json::parse(trimmed);
    if (json.is_object() && json.contains("error")) return result;
    if (appendResponseText(json, result.text) ||
        (json.is_object() && json.contains("choices") && json["choices"].is_array())) {
      result.ok = true;
      result.text = trimAscii(result.text);
    }
  } catch (...) {
    // Do not treat a provider's plain-text error page as a successful transcript.
  }
  return result;
}

}  // namespace aoi
