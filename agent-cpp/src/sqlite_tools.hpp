#pragma once
#include <string>

#include "llm_client.hpp"

namespace aoi {

// Read-only SQLite query tool: lets the LLM inspect local SQLite databases
// (notably the VRCX companion app's session store) without write access.
// `configuredDbPath` comes from aoi_config.json's "vrcxDbPath" (may be empty);
// it is used when the LLM does not pass an explicit db_path.
ToolDefinition makeSqlQueryTool(const std::string& configuredDbPath = "");

} // namespace aoi
