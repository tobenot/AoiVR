#include "convert_tools.hpp"

#include "windows_sandbox.hpp"

#include <string>

namespace aoi {

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
      "returned by sql_query on VRCX's cookies table; "
      "json_query_file - read a JSON document from a FILE (path relative to the sandbox "
      "workspace, e.g. \"out\\\\fetch_123.txt\"), then query/filter/count it. This is the "
      "sandbox-safe way to process large API dumps saved by fetch/bash/sql_query - PowerShell "
      "and python are NOT available inside the sandbox. Optional 'filter' is a single "
      "comparison applied to each array element (\"key==value\" keeps matches, "
      "\"key!=value\" keeps non-matches, e.g. \"location!=offline\"), optional 'count' "
      "returns just the number of results instead of the data.";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"action",
            {{"type", "string"},
             {"enum",
              nlohmann::json::array(
                  {"base64_decode", "base64_encode", "json_query", "json_query_file"})},
             {"description", "Which transformation to apply."}}},
           {"data",
            {{"type", "string"},
             {"description",
              "The input: base64 text, UTF-8 text, a JSON document, or (for "
              "json_query_file) a workspace-relative file path like out\\\\fetch_123.txt"}}},
           {"path",
            {{"type", "string"},
             {"description",
              "For json_query/json_query_file: dot path with optional [n] subscripts "
              "(e.g. \"0.Value\"). Empty means the whole document."}}},
           {"filter",
            {{"type", "string"},
             {"description",
              "For json_query_file only: \"key==value\" or \"key!=value\" applied to each "
              "array element (e.g. \"location!=offline\" to count online friends)."}}},
           {"count",
            {{"type", "boolean"},
             {"description",
              "For json_query_file only: return just the result count instead of the data."}}}}},
      {"required", nlohmann::json::array({"action", "data"})},
  };
  t.execute = [](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    // Execute inside the sandbox (aligned with Codex: the agent process never
    // runs model-controlled operations itself).
    const std::string reply =
        sandboxExecute(nlohmann::json{{"op", "convert"},
                                      {"fn", args.value("action", "")},
                                      {"value", args.value("data", "")},
                                      {"path", args.value("path", "")},
                                      {"filter", args.value("filter", "")},
                                      {"count", args.value("count", false)}}
                           .dump());
    auto j = nlohmann::json::parse(reply, nullptr, false);
    if (!j.is_discarded() && j.is_object() && j.contains("ok") &&
        j["ok"].is_boolean() && j["ok"].get<bool>()) {
      return nlohmann::json{{"content", j.value("output", "")}};
    }
    return nlohmann::json{
        {"content", !j.is_discarded() && j.is_object()
                        ? j.value("error", "(sandbox error)")
                        : reply}};
  };
  return t;
}

} // namespace aoi
