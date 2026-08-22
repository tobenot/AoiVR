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

HttpClient::~HttpClient() { delete impl_; }

void* HttpClient::handle() {
  if (!impl_) {
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

  // Timeouts: connect 15s (fast fail on unreachable provider), low-speed guard
  // 45s (no data transferred for 45s = dead stream, e.g. a hung 401). SSE stays
  // open as long as data keeps flowing, so long replies are unaffected.
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
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
    // Transport failure (DNS, TCP connect, TLS handshake, timeout, reset...):
    // capture curl's error string + the raw OS errno so callers can surface a
    // real diagnosis instead of a generic "network error". HTTP-level failures
    // (4xx/5xx) leave transportError empty — the body carries that story.
    if (res != CURLE_OK) {
      const char* errStr = curl_easy_strerror(res);
      result.transportError = errStr ? errStr : "unknown curl error";
      long osErr = 0;
      curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &osErr);
      result.osError = osErr;
    }
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
