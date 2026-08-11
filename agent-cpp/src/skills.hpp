#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aoi {

// Agent Skills, aligned with the open standard used by opencode / Codex /
// Claude Code: a skill is a directory containing a `SKILL.md` file (YAML
// frontmatter with `name` + `description`, then markdown instructions), with
// optional scripts/references/assets next to it. Progressive disclosure: only
// the name+description catalog is injected into the system prompt (bounded by
// a character budget); the model reads the full `SKILL.md` on demand with its
// file tools, exactly like Codex's host skills.
//
// Two skill roots (mirroring the two registry files):
//   - system:  <exeDir>/sandbox/skills/       read-only for the sandbox user
//   - user:    <exeDir>/sandbox/skills_user/  writable by the sandbox user
//
// Enablement rules come from the "skills" section of both registry JSON
// files (Codex SkillsConfig semantics: name/path selectors + enabled flag,
// later rules win).

enum class SkillScope { System, User };

struct SkillDef {
  std::string name;
  std::string description;
  std::string shortDescription;  // metadata.short-description (optional)
  std::string relPath;           // path shown to the model, e.g. skills/foo/SKILL.md
  std::string absPath;           // full path used internally
  SkillScope scope = SkillScope::System;
  bool enabled = true;
};

struct SkillRule {
  std::string name;
  bool enabled;
};

struct SkillRules {
  bool includeInstructions = true;
  std::vector<SkillRule> rules;  // ordered; later rules override earlier ones
};

// ---- frontmatter parsing (Codex parser.rs semantics) ----

// Parse `---`-delimited frontmatter; returns true and fills name/description/
// short_description on success. `defaultName` is used when `name` is absent
// (Codex falls back to the directory name).
struct ParsedSkillFrontmatter {
  std::string name;
  std::string description;
  std::string shortDescription;
};
bool parseSkillFrontmatter(const std::string& contents,
                           const std::string& defaultName,
                           ParsedSkillFrontmatter& out);

// Validated skill name per the open standard:
// `^[a-z0-9]+(-[a-z0-9]+)*$`, 1-64 chars, matches the directory name.
bool isValidSkillName(const std::string& name);

// ---- discovery ----

// Recursively scan `root` (max depth, mirroring Codex MAX_SCAN_DEPTH) for
// `*/SKILL.md` files and parse their frontmatter. Invalid files (missing
// frontmatter/description, bad names) are skipped.
std::vector<SkillDef> scanSkillsDir(const std::string& root, int maxDepth,
                                    SkillScope scope);

// Discover system + user skill roots.
std::vector<SkillDef> loadSkills(const std::string& systemDir,
                                 const std::string& userDir, int maxDepth = 6);

// ---- enablement rules (Codex SkillsConfig semantics) ----

// Load the "skills" section from both registry files and merge them: system
// rules first, then user rules appended (later rules override earlier ones).
SkillRules loadSkillRules(const std::string& systemRegistryPath,
                          const std::string& userRegistryPath);

// Apply rules in order; skills matched by a later rule keep its setting.
void applySkillRules(std::vector<SkillDef>& skills, const SkillRules& rules);

// ---- rendering (Codex render.rs budget semantics) ----

// Budget for the initial skills catalog: 8000 characters (Codex default when
// the context window is unknown).
inline constexpr int kSkillCatalogCharBudget = 8000;
// Per-entry description cap (Codex MAX_CATALOG_SKILL_DESCRIPTION_CHARS).
inline constexpr int kSkillDescriptionMaxChars = 1024;

// Render the "## Skills" section injected into the system prompt: intro,
// "### Available skills" list (name: description (file: <relPath>)) and, when
// enabled, the progressive-disclosure usage instructions. Truncation ladder
// mirrors Codex: full lines -> descriptions shortened round-robin -> entries
// omitted, always within kSkillCatalogCharBudget.
std::string renderAvailableSkills(const std::vector<SkillDef>& skills,
                                  bool includeInstructions = true,
                                  int budget = kSkillCatalogCharBudget);

// Create the skill root directories when missing.
void ensureSkillDirs(const std::string& systemDir, const std::string& userDir);

} // namespace aoi
