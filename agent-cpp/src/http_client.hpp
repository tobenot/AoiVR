#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aoi {

// Streaming HTTPS client built on libcurl (see THIRD_PARTY_NOTICES.md section
// 4). Used for the LLM and TTS SSE streams. Blocking per request.
//
// One HttpClient per long-lived session (LlmSession / MiMoTTS): the CURL
// handle is kept alive so TCP + TLS connections are REUSED across requests
// (keep-alive). Creating a fresh handle per request costs a DNS + TCP + TLS
// handshake every time (~seconds through a proxy), which dominates latency
// for frequent small requests such as interpretation windows.
// NOT thread-safe: use one instance per session (each session calls it from
// a single worker at a time).
class HttpClient {
 public:
  HttpClient() = default;
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;
  using OnData = std::function<void(const char* data, size_t len)>;
  // Return true to abort the in-flight transfer (checked frequently by curl).
  using CancelCheck = std::function<bool()>;

  struct StreamCtx {
    OnData onData;
    CancelCheck cancel;
    std::string body;  // populated when onData is null
  };

  struct Result {
    long status = 0;
    std::string body;  // only populated when onData is not provided
    // Response headers, keys lowercased (e.g. "retry-after", "content-type").
    // Used by the caller for Retry-After backoff handling.
    std::vector<std::pair<std::string, std::string>> headers;
    // curl error text on transport failure (status <= 0), empty otherwise.
    std::string error;
    // Transport-level failure detail: the curl error string (CURLE_* code +
    // human message, e.g. "Couldn't resolve host 'api.x.com'"). Empty when
    // the transfer itself succeeded (any HTTP status, even 4xx/5xx).
    // Kept separately for the user-facing diagnostic path.
    std::string transportError;
    long osError = 0;  // CURLINFO_OS_ERRNO snapshot (0 when unknown)
    // libcurl CURLcode of the transfer (CURLE_OK on success). A non-OK code
    // combined with a positive HTTP status means the connection broke in the
    // middle of the response body ("mid-stream break") - the response was NOT
    // complete even though headers were received.
    int curlCode = 0;
  };

  // POST with streaming response. headers includes "Content-Type: ..." etc.
  // The Authorization header must be supplied by the caller. When onData is
  // null, the full body is accumulated. When cancel returns true, the transfer
  // is aborted promptly (returns status -1). timeoutMs > 0 applies a hard
  // total + connect timeout (the fetch tool uses it; the LLM/TTS streams pass
  // 0 and keep the wait-indefinitely behavior).
  Result postStream(const std::string& url, const std::vector<std::string>& headers,
                    const std::string& body, OnData onData = nullptr,
                    CancelCheck cancel = nullptr, int timeoutMs = 0);

  // Simple GET returning the full body (used by the fetch tool; runs in the
  // agent process - the sandbox's CreateProcessAsUserW children cannot use
  // schannel, SEC_E_NO_CREDENTIALS).
  Result get(const std::string& url, const std::vector<std::string>& headers = {},
             int timeoutMs = 0);

  // Simple POST returning full body.
  Result post(const std::string& url, const std::vector<std::string>& headers,
              const std::string& body, int timeoutMs = 0);

 private:
  void* handle();
  struct Impl;
  Impl* impl_ = nullptr;  // lazily created persistent CURL handle
};

} // namespace aoi
