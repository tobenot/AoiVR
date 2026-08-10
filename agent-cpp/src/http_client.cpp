#include "http_client.hpp"

#include <curl/curl.h>

#include <cstring>
#include <mutex>

namespace aoi {

namespace {

// Called by libcurl for each chunk of the response body.
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<HttpClient::StreamCtx*>(userdata);
  const size_t bytes = size * nmemb;
  if (ctx->onData) {
    ctx->onData(ptr, bytes);
  } else {
    ctx->body.append(ptr, bytes);
  }
  return bytes;
}

// Progress callback: return nonzero to abort the transfer.
int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* ctx = static_cast<HttpClient::StreamCtx*>(userdata);
  if (ctx->cancel && ctx->cancel()) return 1;
  return 0;
}

} // namespace

// Persistent CURL handle: keeps the TCP/TLS connection alive across requests
// (curl reuses the connection on subsequent perform() calls on the same
// handle). Destroyed with the HttpClient.
struct HttpClient::Impl {
  CURL* curl = nullptr;
  ~Impl() {
    if (curl) curl_easy_cleanup(curl);
  }
};

namespace {
// libcurl requires curl_global_init() before any other curl call, and in
// multi-threaded programs it MUST complete before threads start using curl.
// The agent's first request can originate from a worker thread, so initialize
// lazily but thread-safely with std::call_once.
std::once_flag g_curlInitFlag;
void ensureCurlInit() {
  std::call_once(g_curlInitFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}
} // namespace

HttpClient::~HttpClient() { delete impl_; }

void* HttpClient::handle() {
  if (!impl_) {
    ensureCurlInit();
    impl_ = new Impl();
    impl_->curl = curl_easy_init();
  }
  return impl_->curl;
}

HttpClient::Result HttpClient::postStream(const std::string& url,
                                          const std::vector<std::string>& headers,
                                          const std::string& body, OnData onData,
                                          CancelCheck cancel) {
  StreamCtx ctx;
  ctx.onData = std::move(onData);
  ctx.cancel = std::move(cancel);

  CURL* curl = static_cast<CURL*>(handle());
  if (!curl) {
    Result r;
    r.status = -1;
    return r;
  }

  curl_easy_reset(curl);  // clear the previous request's options, keep the connection
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  // No Accept-Encoding: gzip: the SSE stream must stay raw. Advertising gzip
  // made opencode.ai return a compressed stream that this libcurl build failed
  // to decompress in the streaming path (HTTP 500 / hangs). Plain identity
  // encoding keeps the raw SSE bytes flowing.
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");

  // Timeouts: connect 15s (fast fail on unreachable provider). NO low-speed
  // guard: the opencode gateway's SSE stream routinely pauses for minutes
  // between the tool-call announce chunk and the argument deltas (the model
  // deliberates on the arguments / gateway buffering). A low-speed guard
  // (45s, then 300s) killed those requests mid-stream and lost the tool
  // arguments (announce-only bug); Python's urllib has no such guard and
  // never failed. SSE stays open as long as the connection lives, so a dead
  // stream is only detected by the connect timeout / cancellation.
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Headers.
  curl_slist* headerList = nullptr;
  for (const auto& h : headers) {
    headerList = curl_slist_append(headerList, h.c_str());
  }
  if (headerList) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
  }

  // Follow redirects (some providers 301/302).
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

  // Windows-native TLS (Schannel), no OpenSSL needed.
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  const CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  Result result;
  // CURLE_ABORTED_BY_CALLBACK means our cancel check fired.
  if (res == CURLE_ABORTED_BY_CALLBACK) {
    result.status = -1;
  } else {
    result.status = static_cast<long>(status > 0 ? status : (res == CURLE_OK ? 0 : -1000));
  }
  result.body = std::move(ctx.body);

  if (headerList) curl_slist_free_all(headerList);
  return result;
}

HttpClient::Result HttpClient::post(const std::string& url,
                                    const std::vector<std::string>& headers,
                                    const std::string& body) {
  return postStream(url, headers, body, nullptr);
}

} // namespace aoi
