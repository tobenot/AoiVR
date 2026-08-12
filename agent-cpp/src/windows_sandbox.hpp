#pragma once
#include <string>
#include <vector>

namespace aoi {

// Windows sandbox for tool execution, aligned with Codex's restricted-token
// sandbox (WRITE_RESTRICTED token + file ACL + sandboxed child process):
//
//   model (tool call text)
//     -> agent process (parse/decide only - never executes model writes)
//       -> aoi-sandbox-helper.exe spawned with a restricted token
//            (the ONLY place writes happen; the OS denies anything outside
//             the sandbox workspace via the token's restricting SIDs)
//
// Policy (per project decision):
//   - writable: ONLY <workdir>/sandbox/   (system-enforced via ACL)
//   - readable: full disk                 (WRITE_RESTRICTED limits writes only)
//   - network:  full access               (no WFP filtering)
//   - failure:  refuse to execute, return the error to the model
//
// The helper executable must sit next to the agent (exe dir / DLL dir).

// Run one tool operation inside the sandbox. `opJson` is a single-line JSON
// document: {"op":"bash|read|write|edit|sql_query|convert", ...op params...}.
// Returns the helper's JSON reply {"ok":true,"output":"..."} or
// {"ok":false,"error":"..."}, or a JSON error object when the sandbox itself
// could not be set up (the caller surfaces it to the model).
std::string sandboxExecute(const std::string& opJson);

// Full path of the sandbox workspace directory (<workdir>/sandbox), created
// on first use. Agent-side writable paths that must survive (logs etc.) are
// NOT placed here; the agent process itself is trusted and never executes
// model-controlled writes (those all go through sandboxExecute).
std::string sandboxWorkspacePath();

// Directories the sandbox user should be able to READ at startup (registered
// from aoi_config.json "sandbox.read_dirs"). Relative entries resolve against
// the agent exe dir. Applied (granted read+execute, inherited) by
// ensureSandboxReady on the next sandbox call. Call once at agent start.
void setSandboxReadDirs(const std::vector<std::string>& dirs);

} // namespace aoi
