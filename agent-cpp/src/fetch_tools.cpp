#include "fetch_tools.hpp"

#include <string>

#include "http_client.hpp"

namespace aoi {

ToolDefinition makeFetchTool() {
  ToolDefinition t;
  t.name = "fetch";
  t.label = "fetch";
  t.description =
      "Fetch a URL over HTTP/HTTPS (GET) and return the response text. Use this "
      "for ALL network requests (web APIs, web pages) - the sandbox bash "
      "environment cannot do TLS (curl exits with error 35). "
      "Response is limited to 30KB and 15 seconds. If the response is JSON, it "
      "comes back verbatim.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{{"url",
                       {{"type", "string"},
                        {"description", "http:// or https:// URL to GET"}}}}},
      {"required", nlohmann::json::array({"url"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    try {
      const std::string url = args.value("url", "");
      if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return nlohmann::json{{"content",
                               "(fetch: only http:// and https:// URLs are "
                               "allowed)"}};
      }
      HttpClient http;
      const auto res = http.get(url);
      if (res.status <= 0) {
        return nlohmann::json{{"content",
                               "(fetch failed: " + res.error + ")"}};
      }
      if (res.status >= 400) {
        return nlohmann::json{{"content",
                               "(fetch: HTTP " + std::to_string(res.status) +
                                   ")"}};
      }
      std::string body = res.body;
      if (body.size() > 30000) {
        body.resize(30000);
        body += "\n...(truncated: response exceeds 30KB)";
      }
      if (body.empty()) body = "(fetch: empty response)";
      return nlohmann::json{{"content", body}};
    } catch (const std::exception& ex) {
      return nlohmann::json{{"content",
                             std::string("(fetch failed: ") + ex.what() + ")"}};
    }
  };
  return t;
}

} // namespace aoi
