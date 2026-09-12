#include <cstdio>
#include <string>

#include "remote_asr.hpp"

using namespace aoi;

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++failures;                                                          \
    }                                                                        \
  } while (0)

int main() {
  RemoteAsrAudio wav;
  CHECK(parseRemoteAsrAudioDataUrl("data:audio/wav;base64,UklGRgAAAA==", wav));
  CHECK(wav.data == "UklGRgAAAA==");
  CHECK(wav.format == "wav");

  RemoteAsrAudio mp3;
  CHECK(parseRemoteAsrAudioDataUrl("data:audio/mpeg;base64,/+MYxAAAAA==", mp3));
  CHECK(mp3.data == "/+MYxAAAAA==");
  CHECK(mp3.format == "mp3");

  RemoteAsrAudio invalid;
  CHECK(!parseRemoteAsrAudioDataUrl("data:image/png;base64,AAAA", invalid));
  CHECK(!parseRemoteAsrAudioDataUrl("", invalid));

  const auto request = buildRemoteAsrRequest(
      "mimo-v2.5", "请把这段语音转成文字，只输出文字。", wav);
  CHECK(request["model"] == "mimo-v2.5");
  CHECK(request["stream"] == false);
  CHECK(request["messages"].is_array());
  CHECK(request["messages"].size() == 1);
  const auto& content = request["messages"][0]["content"];
  CHECK(content.is_array());
  CHECK(content[0]["type"] == "text");
  CHECK(content[1]["type"] == "input_audio");
  CHECK(content[1]["input_audio"]["data"] == "UklGRgAAAA==");
  CHECK(content[1]["input_audio"]["format"] == "wav");

  {
    const auto parsed = parseRemoteAsrResponse(
        R"({"choices":[{"message":{"content":"你好，Aoi。"}}]})");
    CHECK(parsed.ok);
    CHECK(parsed.text == "你好，Aoi。");
  }

  {
    const auto parsed = parseRemoteAsrResponse(
        R"({"text":"这是转写结果"})");
    CHECK(parsed.ok);
    CHECK(parsed.text == "这是转写结果");
  }

  {
    const auto parsed = parseRemoteAsrResponse(
        "data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\n"
        "data: [DONE]\n\n");
    CHECK(parsed.ok);
    CHECK(parsed.text == "你好");
  }

  {
    const auto parsed = parseRemoteAsrResponse(
        R"({"choices":[{"message":{"content":[{"type":"text","text":"分段"},{"type":"text","text":"文本"}]}}]})");
    CHECK(parsed.ok);
    CHECK(parsed.text == "分段文本");
  }

  CHECK(!parseRemoteAsrResponse(R"({"error":{"message":"bad audio"}})").ok);
  CHECK(!parseRemoteAsrResponse("not-json").ok);

  if (failures == 0) {
    std::printf("ALL REMOTE ASR TESTS PASSED\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
