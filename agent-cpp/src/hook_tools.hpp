#pragma once
#include <string>

#include "llm_client.hpp"

namespace aoi {

class HookScheduler;

// Generic timer-hook management tool (create/list/cancel/pause/resume).
// The scheduler instance is supplied so the tool routes through the same
// persistence + guardrail machinery. Generic scheduling: no business logic.
ToolDefinition makeHookManageTool(HookScheduler* scheduler);

} // namespace aoi
