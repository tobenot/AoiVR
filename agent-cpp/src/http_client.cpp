#include "http_client.hpp"

#include <curl/curl.h>

#include <cctype>
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

// Header callback: collect response headers (lowercased keys) so the caller
// can honor Retry-After. Status lines ("HTTP/1.1 429 ...") are skipped.
size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::vector<std::pair<std::string, std::string>>*>(userdata);
  const size_t n = size * nmemb;
  std::string line(ptr, n);
  if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0)
    line.resize(line.size() - 2);
  const size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t b = val.find_first_not_of(" \t");
    if (b != std::string::npos) val = val.substr(b);
    const size_t e = val.find_last_not_of(" \t");
    if (e != std::string::npos) val = val.substr(0, e + 1);
    out->push_back({key, val});
  }
  return n;
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

// Hard total + connect timeouts (used by the fetch tool; 0 = no timeout).
void applyTimeout(CURL* curl, int timeoutMs) {
  if (timeoutMs <= 0) return;
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>((std::min)(timeoutMs, 5000)));
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
                                          CancelCheck cancel, int timeoutMs) {
  StreamCtx ctx;
  ctx.onData = std::move(onData);
  ctx.cancel = std::move(cancel);
  std::vector<std::pair<std::string, std::string>> resultHeaders;

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
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resultHeaders);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  // No Accept-Encoding: gzip: the SSE stream must stay raw. Advertising gzip
  // made opencode.ai return a compressed stream that this libcurl build failed
  // to decompress in the streaming path (HTTP 500 / hangs). Plain identity
  // encoding keeps the raw SSE bytes flowing.
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");

  // NO connect timeout and NO total timeout: aligned with opencode on Bun,
  // which waits indefinitely for slow streams...
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // ...BUT a silently dead stream must not wedge the agent forever: abort
  // when fewer than 1 byte/sec flows (either direction) for 90 consecutive
  // seconds. The abort surfaces as CURLE_OPERATION_TIMEDOUT, which the
  // caller's stream-retry layer re-issues on a FRESH connection. Live
  // incident: a dead stream hung two consecutive turns for minutes with zero
  // bytes and no recovery until process restart.
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

  // Headers.
  curl_slist* headerList = nullptr;
  for (const auto& h : headers) {
    headerList = curl_slist_append(headerList, h.c_str());
  }
  if (headerList) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
  }

  // Follow redirects (some providers 301/302), but never off http/https
  // (a 302 to ftp:// or file:// must not be followed).
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));

  // Windows-native TLS (Schannel), no OpenSSL needed.
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  applyTimeout(curl, timeoutMs);

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
    // (4xx/5xx) leave these transport fields empty — the body carries that story.
    if (res != CURLE_OK) {
      const char* errStr = curl_easy_strerror(res);
      result.error = errStr ? errStr : "unknown curl error";
      result.transportError = result.error;
      long osErr = 0;
      curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &osErr);
      result.osError = osErr;
    }
  }
  result.curlCode = static_cast<int>(res);
  result.body = std::move(ctx.body);
  result.headers = std::move(resultHeaders);

  if (headerList) curl_slist_free_all(headerList);
  return result;
}

HttpClient::Result HttpClient::post(const std::string& url,
                                    const std::vector<std::string>& headers,
                                    const std::string& body, int timeoutMs) {
  return postStream(url, headers, body, nullptr, nullptr, timeoutMs);
}

HttpClient::Result HttpClient::get(const std::string& url,
                                   const std::vector<std::string>& headers,
                                   int timeoutMs) {
  StreamCtx ctx;
  std::vector<std::pair<std::string, std::string>> resultHeaders;

  CURL* curl = static_cast<CURL*>(handle());
  if (!curl) {
    Result r;
    r.status = -1;
    return r;
  }

  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resultHeaders);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  applyTimeout(curl, timeoutMs);

  curl_slist* headerList = nullptr;
  for (const auto& h : headers) {
    headerList = curl_slist_append(headerList, h.c_str());
  }
  if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

  const CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  Result result;
  if (res == CURLE_ABORTED_BY_CALLBACK) {
    result.status = -1;
  } else {
    result.status = static_cast<long>(status > 0 ? status : (res == CURLE_OK ? 0 : -1000));
    if (res != CURLE_OK) result.error = curl_easy_strerror(res);
  }
  result.curlCode = static_cast<int>(res);
  result.body = std::move(ctx.body);
  result.headers = std::move(resultHeaders);

  if (headerList) curl_slist_free_all(headerList);
  return result;
}

} // namespace aoi
