#pragma once
#include <string>

#include "llm_client.hpp"

namespace aoi {

// Data transformation tool: base64 decode/encode and JSON path lookup.
ToolDefinition makeConvertTool();

} // namespace aoi
