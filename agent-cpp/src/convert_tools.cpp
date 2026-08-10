#include "convert_tools.hpp"

#include "base64.hpp"

#include <cctype>
#include <string>

namespace aoi {

namespace {

// Render decoded bytes as UTF-8 text when printable (or valid UTF-8), else as
// hex so nothing is silently mangled.
std::string renderDecoded(const std::vector<uint8_t>& bytes) {
  bool text = true;
  for (uint8_t b : bytes) {
    if (b == 0x09 || b == 0x0A || b == 0x0D) continue;
    if (b < 0x20 || b > 0x7E) {
      text = false;
      break;
    }
  }
  if (text) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  static const char* kHex = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 3);
  for (uint8_t b : bytes) {
    hex += kHex[b >> 4];
    hex += kHex[b & 0x0F];
    hex += ' ';
  }
  return "(binary, hex) " + hex;
}

// Walk a dot-separated path with array subscripts, e.g. "0.Value",
// "favorites", "rows[0].value", "items.0.name". Both bare numbers and [n]
// forms index into arrays. Returns a null JSON value when any segment does
// not resolve.
nlohmann::json resolvePath(const nlohmann::json& root, const std::string& path) {
  nlohmann::json cur = root;
  size_t i = 0;
  while (i < path.size()) {
    const size_t dot = path.find('.', i);
    const std::string token = path.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
    if (token.empty()) return nullptr;
    if (token.front() == '[' && token.back() == ']') {
      // Array subscript segment: "[n]".
      std::string idxStr = token.substr(1, token.size() - 2);
      if (idxStr.empty() || idxStr.find_first_not_of("0123456789") != std::string::npos) {
        return nullptr;
      }
      const int idx = std::atoi(idxStr.c_str());
      if (!cur.is_array() || idx < 0 || idx >= static_cast<int>(cur.size())) return nullptr;
      cur = cur[static_cast<size_t>(idx)];
    } else if (!token.empty() && token.find_first_not_of("0123456789") == std::string::npos) {
      // Bare numeric segment: "0" also indexes into an array.
      const int idx = std::atoi(token.c_str());
      if (!cur.is_array() || idx < 0 || idx >= static_cast<int>(cur.size())) return nullptr;
      cur = cur[static_cast<size_t>(idx)];
    } else {
      if (!cur.is_object() || !cur.contains(token)) return nullptr;
      cur = cur[token];
    }
    if (dot == std::string::npos) break;
    i = dot + 1;
  }
  return cur;
}

} // namespace

ToolDefinition makeConvertTool() {
  ToolDefinition t;
  t.name = "convert";
  t.label = "convert";
  t.description =
      "Data transformation utility. Actions: "
      "base64_decode - decode a base64 string to UTF-8 text (non-printable bytes are shown as hex); "
      "base64_encode - encode UTF-8 text to base64; "
      "json_query - parse a JSON document and extract the value at a dot path with optional [n] "
      "array subscripts (e.g. \"0.Value\", \"favorites\", \"items[0].name\"). An empty path "
      "returns the whole document pretty-printed. Useful for decoding the base64 cookie blob "
      "returned by sql_query on VRCX's cookies table.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"action",
            {{"type", "string"},
             {"enum", nlohmann::json::array({"base64_decode", "base64_encode", "json_query"})},
             {"description", "Which transformation to apply."}}},
           {"data",
            {{"type", "string"},
             {"description", "The input: base64 text, UTF-8 text, or a JSON document."}}},
           {"path",
            {{"type", "string"},
             {"description",
              "For json_query only: dot path with optional [n] subscripts (e.g. \"0.Value\"). "
              "Empty means the whole document."}}}}},
      {"required", nlohmann::json::array({"action", "data"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    try {
      const std::string action = args.value("action", "");
      const std::string data = args.value("data", "");
      const std::string path = args.value("path", "");

      if (action == "base64_decode") {
        std::vector<uint8_t> out;
        if (!base64Decode(data, out)) {
          return nlohmann::json{{"content", "(convert: invalid base64 input)"}};
        }
        return nlohmann::json{{"content", renderDecoded(out)}};
      }

      if (action == "base64_encode") {
        return nlohmann::json{{"content", base64Encode(data)}};
      }

      if (action == "json_query") {
        nlohmann::json root = nlohmann::json::parse(data, nullptr, false);
        if (root.is_discarded()) {
          return nlohmann::json{{"content", "(convert: invalid JSON document)"}};
        }
        nlohmann::json value = path.empty() ? root : resolvePath(root, path);
        if (value.is_null()) {
          return nlohmann::json{
              {"content", "(convert: path not found in JSON document: " + path + ")"}};
        }
        std::string text = value.is_string() ? value.get<std::string>()
                                             : value.dump(value.is_object() || value.is_array() ? 2 : -1);
        constexpr size_t kMax = 30000;
        if (text.size() > kMax) {
          text.resize(kMax);
          text += "\n...(truncated)";
        }
        return nlohmann::json{{"content", text}};
      }

      return nlohmann::json{{"content", "(convert: unknown action: " + action + ")"}};
    } catch (const std::exception& ex) {
      return nlohmann::json{{"content", std::string("(convert failed: ") + ex.what() + ")"}};
    }
  };
  return t;
}

} // namespace aoi
