#pragma once
#include "llm_client.hpp"

namespace aoi {

// Network fetch tool: runs in the AGENT process (real user) because the
// sandboxed children (CreateProcessAsUserW) cannot initialize schannel
// (SEC_E_NO_CREDENTIALS - Windows limitation of that spawn path, verified).
// The model is directed here instead of `curl` in bash for ALL network
// requests; bash curl still works for plain HTTP or local endpoints.
ToolDefinition makeFetchTool();

} // namespace aoi
