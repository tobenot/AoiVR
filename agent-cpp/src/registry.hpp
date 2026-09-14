#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aoi {

// Tool/skill registry: two JSON files decide which tools are registered with
// the agent.
//   - system_registry.json (next to aoi_config.json): controlled by the agent
//     developer; the sandbox user is read-only there (workdir protection), so
//     the model can never re-enable a tool the developer disabled.
//   - sandbox/user_registry.json (inside the sandbox workspace): editable by
//     the model through the write tool; user entries override same-name
//     system entries, so the user (or the model on their behalf) can disable
//     tools.
// Skill enablement rules live in the "skills" section of the same files but
// are parsed by skills.hpp (Codex SkillsConfig semantics).
// Missing registry files keep the legacy behavior (everything enabled).

struct ToolEntry {
  std::string name;
  bool enabled = true;
  bool enabledSet = false;  // "enabled" explicitly present in the JSON
  bool hasLabel = false;
  bool hasDescription = false;
  bool hasParameters = false;
  std::string label;
  std::string description;
  nlohmann::json parameters;
};

struct ToolRegistry {
  std::vector<ToolEntry> tools;

  // Effective tool entry for `name` (merged system+user), or nullptr.
  const ToolEntry* findTool(const std::string& name) const;
  // True unless a merged entry explicitly sets enabled=false. Tools not
  // listed in any registry stay enabled (legacy behavior).
  bool toolEnabled(const std::string& name) const;
};

// Parse a single registry JSON file. Missing/invalid file => empty registry.
ToolRegistry loadRegistryFile(const std::string& path);

// Load system + user registries and merge: same-name user entries override
// system entries (explicit fields only), user-only entries are appended.
ToolRegistry loadRegistries(const std::string& systemPath,
                            const std::string& userPath);

// Create the default registry files when they don't exist yet.
void ensureDefaultRegistries(const std::string& systemPath,
                             const std::string& userPath);

} // namespace aoi
