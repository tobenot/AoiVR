#include "fetch_tools.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "agent_utils.hpp"
#include "http_client.hpp"

namespace aoi {

namespace {

std::string agentExeDir() {
  char buf[MAX_PATH]{};
  if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
    std::string dir(buf);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) return dir.substr(0, slash + 1);
  }
  return "";
}

// Save the FULL response body into the sandbox workspace out\ dir so nothing
// is lost when only a size-capped head is returned. Returns the
// workspace-relative path ("out\fetch_..._.txt"), or "" on failure.
std::string saveFullBody(const std::string& body) {
  const std::string exe = agentExeDir();
  if (exe.empty()) return "";
  const std::string dir = exe + "sandbox\\out";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string rel =
      "out\\fetch_" + std::to_string(GetTickCount64()) + ".txt";
  std::ofstream f(dir + "\\" + rel.substr(4), std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return "";
  f.write(body.data(), static_cast<std::streamsize>(body.size()));
  f.close();
  return rel;
}

// Parse+validate the curl -H style header array: every entry must be a string
// containing a colon. Returns false with a message on the first bad entry.
bool parseHeaders(const nlohmann::json& args, std::vector<std::string>* out,
                  std::string* error) {
  if (!args.contains("headers") || args["headers"].is_null()) return true;
  if (!args["headers"].is_array()) {
    *error = "(fetch: 'headers' must be an array of \"Name: value\" strings)";
    return false;
  }
  for (const auto& h : args["headers"]) {
    if (!h.is_string()) {
      *error = "(fetch: 'headers' entries must be \"Name: value\" strings)";
      return false;
    }
    const std::string line = h.get<std::string>();
    if (line.find(':') == std::string::npos) {
      *error = "(fetch: header \"" + line + "\" has no ':' - use \"Name: value\" format)";
      return false;
    }
    out->push_back(line);
  }
  return true;
}

long clampLong(long v, long lo, long hi) {
  return (std::max)(lo, (std::min)(hi, v));
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

} // namespace

ToolDefinition makeFetchTool() {
  ToolDefinition t;
  t.name = "fetch";
  t.label = "fetch";
  t.description =
      "Fetch a URL over HTTP/HTTPS with full curl-equivalent control: custom "
      "request headers (including cookies), GET or POST with a body, configurable "
      "timeout and response-size limits. Use this for ALL network requests "
      "(web APIs, web pages, authenticated endpoints) - the sandbox bash "
      "environment CANNOT do TLS (curl exits with error 35), so fetch is the "
      "only reliable way to reach the internet. Pass custom cookies as a header "
      "line, e.g. [\"Cookie: session=abc123\"]. Returns HTTP status, response "
      "headers (e.g. Set-Cookie) and the body. Responses longer than max_bytes "
      "are NOT lost: the full body is saved to a file under the sandbox "
      "workspace out\\ directory and the head is returned with its path - use "
      "the read tool with offset to page through the rest.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"url", {{"type", "string"}, {"description", "http:// or https:// URL"}}},
           {"headers",
            {{"type", "array"},
             {"items", {{"type", "string"}}},
             {"description",
              "Optional request headers, curl -H style: [\"Authorization: Bearer "
              "xxx\", \"Cookie: a=b; c=d\"]"}}},
           {"method",
            {{"type", "string"},
             {"enum", {"GET", "POST"}},
             {"description", "HTTP method (default GET)"}}},
           {"body",
            {{"type", "string"}, {"description", "Request body for POST (max 1MB)"}}},
           {"content_type",
            {{"type", "string"},
             {"description", "Content-Type for POST (default application/json)"}}},
           {"timeout_ms",
            {{"type", "integer"},
             {"description", "Timeout in milliseconds (1-60000, default 15000)"}}},
           {"max_bytes",
            {{"type", "integer"},
             {"description",
              "Max response bytes returned (1024-1048576, default 30000)"}}}}},
      {"required", nlohmann::json::array({"url"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    try {
      const std::string url = args.value("url", "");
      if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return nlohmann::json{{"status", 0},
                              {"content", "(fetch: only http:// and https:// URLs are allowed)"}};
      }
      std::string method = args.value("method", "GET");
      for (auto& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (method != "GET" && method != "POST") {
        return nlohmann::json{{"status", 0},
                              {"content", "(fetch: method must be GET or POST)"}};
      }

      std::vector<std::string> headers;
      std::string err;
      if (!parseHeaders(args, &headers, &err)) {
        return nlohmann::json{{"status", 0}, {"content", err}};
      }

      const long timeoutMs =
          clampLong(args.value("timeout_ms", 15000), 1000, 60000);
      const size_t maxBytes = static_cast<size_t>(
          clampLong(args.value("max_bytes", 30000), 1024, 1024 * 1024));

      std::string body = args.value("body", "");
      constexpr size_t kMaxBodyBytes = 1024 * 1024;
      if (body.size() > kMaxBodyBytes) {
        return nlohmann::json{{"status", 0},
                              {"content", "(fetch: body exceeds 1MB limit)"}};
      }

      if (method == "POST") {
        std::string ct = args.value("content_type", "application/json");
        if (ct.empty()) ct = "application/json";
        bool hasCt = false;
        for (const auto& h : headers) {
          const size_t colon = h.find(':');
          if (colon != std::string::npos &&
              lower(h.substr(0, colon)) == "content-type") {
            hasCt = true;
            break;
          }
        }
        if (!hasCt) headers.push_back("Content-Type: " + ct);
      }

      HttpClient http;
      const HttpClient::Result res = method == "POST"
                                         ? http.post(url, headers, body, static_cast<int>(timeoutMs))
                                         : http.get(url, headers, static_cast<int>(timeoutMs));

      nlohmann::json out{{"status", res.status}};
      // Response headers, merged (repeated keys like multiple Set-Cookie are
      // joined) so the model can chain cookies across requests.
      if (!res.headers.empty()) {
        std::map<std::string, std::string> merged;
        for (const auto& [k, v] : res.headers) {
          if (merged.count(k)) merged[k] += ", " + v;
          else merged[k] = v;
        }
        out["headers"] = merged;
      }
      if (res.status <= 0) {
        out["content"] = "(fetch failed: " + res.error + ")";
        return out;
      }
      std::string b = res.body;
      if (b.size() > maxBytes) {
        // Nothing is discarded: the full body goes to sandbox\out\ and the
        // head is returned with the path. Cut on a UTF-8 boundary so the
        // stored head stays valid for JSON serialization downstream.
        const size_t fullSize = b.size();
        const std::string saved = saveFullBody(b);
        utf8SafeTruncate(b, maxBytes);
        if (!saved.empty())
          b += "\n...(truncated: full response " + std::to_string(fullSize) +
               " bytes saved to " + saved +
               " (sandbox workspace); use read with offset to page through)";
        else
          b += "\n...(truncated: response exceeds " + std::to_string(maxBytes) + " bytes)";
      }
      if (b.empty()) b = "(fetch: empty response)";
      out["content"] = b;
      return out;
    } catch (const std::exception& ex) {
      return nlohmann::json{{"status", 0},
                            {"content", std::string("(fetch failed: ") + ex.what() + ")"}};
    }
  };
  return t;
}

} // namespace aoi
