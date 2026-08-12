// fetch tool tests: parameter validation (no network) + optional manual
// live-check mode (aoi-fetch-test <url> [cookie]).
#include <cstdio>
#include <string>

#include "fetch_tools.hpp"
#include "agent_utils.hpp"

namespace {

int failures = 0;

void expectContent(const nlohmann::json& r, const std::string& needle,
                   const std::string& what) {
  const std::string c = r.value("content", "");
  if (c.find(needle) == std::string::npos) {
    std::printf("FAIL: %s -> expected '%s' got: %s\n", what.c_str(),
                needle.c_str(), c.substr(0, 120).c_str());
    ++failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

} // namespace

int main(int argc, char** argv) {
  auto tool = aoi::makeFetchTool();

  // Scheme validation.
  expectContent(tool.execute("", nlohmann::json{{"url", "ftp://x"}}),
                "only http:// and https://", "rejects ftp scheme");
  expectContent(tool.execute("", nlohmann::json{{"url", ""}}),
                "only http:// and https://", "rejects empty url");

  // Method validation.
  expectContent(tool.execute("",
                             nlohmann::json{{"url", "https://example.com"},
                                            {"method", "DELETE"}}),
                "must be GET or POST", "rejects unknown method");
  // Lowercase "post" is normalized to POST and reaches the network layer
  // (127.0.0.1:1 refuses instantly; offline-safe).
  expectContent(tool.execute("",
                             nlohmann::json{{"url", "http://127.0.0.1:1/"},
                                            {"method", "post"},
                                            {"body", "x"}}),
                "fetch failed", "lowercase POST is accepted");

  // Header validation (no network is reached for invalid headers).
  expectContent(tool.execute("",
                             nlohmann::json{{"url", "https://example.com"},
                                            {"headers", {"no-colon-here"}}}),
                "has no ':'", "rejects header without colon");
  // Valid cookie header passes the header parser and reaches the network
  // layer: 127.0.0.1:1 refuses instantly (offline-safe), so the error must be
  // a connection failure, NOT a header error.
  expectContent(tool.execute("",
                             nlohmann::json{{"url", "http://127.0.0.1:1/"},
                                            {"headers", {"Cookie: a=b"}}}),
                "fetch failed", "valid cookie header reaches network");

  // Body size cap (validates before network).
  std::string big(1024 * 1024 + 1, 'x');
  expectContent(tool.execute("",
                             nlohmann::json{{"url", "https://example.com"},
                                            {"method", "POST"},
                                            {"body", big}}),
                "body exceeds 1MB", "rejects oversized body");

  // UTF-8-safe truncation: cutting inside a 3-byte CJK char must back off to
  // the character boundary (the truncated string must re-serialize as JSON).
  {
    std::string s = "ab";
    s += "\xE4\xB8\xAD";  // 中 (3 bytes)
    s += "cd";
    aoi::utf8SafeTruncate(s, 3);  // cuts inside 中
    if (s != "ab") {
      std::printf("FAIL: utf8 truncate mid-char -> got %zu bytes\n", s.size());
      ++failures;
    } else {
      std::puts("ok: utf8 truncate mid-char backs off");
    }
    s = "ab";
    s += "\xE4\xB8\xAD";
    s += "cd";
    aoi::utf8SafeTruncate(s, 5);  // 中 fits whole
    if (s != std::string("ab") + "\xE4\xB8\xAD") {
      std::printf("FAIL: utf8 truncate whole-char -> got %zu bytes\n", s.size());
      ++failures;
    } else {
      std::puts("ok: utf8 truncate keeps whole char");
    }
    // And the truncated text must survive nlohmann serialization.
    const std::string dumped = nlohmann::json(s).dump();
    (void)dumped;
  }

  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  if (failures) return 1;

  // Manual live mode: aoi-fetch-test <url> [cookie-line] [method] [body]
  if (argc > 1) {
    nlohmann::json args{{"url", argv[1]}};
    if (argc > 2) args["headers"] = nlohmann::json::array({argv[2]});
    if (argc > 3) args["method"] = argv[3];
    if (argc > 4) args["body"] = argv[4];
    const auto r = tool.execute("", args);
    std::printf("LIVE status=%d headers=%s\ncontent: %s\n",
                r.value("status", 0),
                r.value("headers", nlohmann::json::object()).dump().c_str(),
                r.value("content", "").c_str());
  }
  return 0;
}
