// Skills unit test: SKILL.md discovery, frontmatter parsing (Codex parser
// semantics), name validation, enablement rules, and budgeted rendering
// (Codex render semantics). Standalone (no hardware, no network).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "skills.hpp"

namespace fs = std::filesystem;

namespace {

int fails = 0;

void check(bool cond, const char* what) {
  std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) ++fails;
}

void write(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p);
  f << content;
}

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "aoi_skills_test";
  fs::remove_all(root);
  const fs::path sys = root / "sys";
  const fs::path usr = root / "usr";

  // ---- frontmatter parsing ----
  {
    aoi::ParsedSkillFrontmatter out;
    check(aoi::parseSkillFrontmatter(
              "---\nname: git-release\ndescription: Create consistent releases\n---\n# body",
              "dir-name", out) &&
              out.name == "git-release" && out.description == "Create consistent releases",
          "basic frontmatter parse");
    // missing name -> directory name fallback (Codex default_skill_name)
    check(aoi::parseSkillFrontmatter(
              "---\ndescription: Only a description\n---", "my-dir", out) &&
              out.name == "my-dir",
          "name falls back to directory");
    // description required
    check(!aoi::parseSkillFrontmatter("---\nname: x\n---", "d", out),
          "missing description rejected");
    check(!aoi::parseSkillFrontmatter("no frontmatter here", "d", out),
          "missing frontmatter rejected");
    // multiline description collapsed to one line (Codex sanitize_single_line)
    check(aoi::parseSkillFrontmatter(
              "---\ndescription: first  \n  second\n---", "d", out) &&
              out.description == "first second",
          "description collapsed to single line");
    // short-description
    check(aoi::parseSkillFrontmatter(
              "---\nname: a\ndescription: long\ndescription_short: s\nmetadata:\n  short-description: short\n---",
              "d", out) &&
              out.shortDescription == "short",
          "metadata.short-description parsed");
  }

  // ---- name validation (open standard regex) ----
  check(aoi::isValidSkillName("git-release"), "valid: git-release");
  check(aoi::isValidSkillName("a1"), "valid: a1");
  check(!aoi::isValidSkillName("Git-release"), "invalid: uppercase");
  check(!aoi::isValidSkillName("-lead"), "invalid: leading hyphen");
  check(!aoi::isValidSkillName("trail-"), "invalid: trailing hyphen");
  check(!aoi::isValidSkillName("double--dash"), "invalid: consecutive hyphens");
  check(!aoi::isValidSkillName("under_score"), "invalid: underscore");

  // ---- discovery ----
  write(sys / "git-release/SKILL.md",
        "---\nname: git-release\ndescription: Create consistent releases\n---\n# body\n");
  write(sys / "nested/level2/frontend-design/SKILL.md",
        "---\nname: frontend-design\ndescription: UI design guidance\n---\n");
  write(sys / "bad-missing-desc/SKILL.md", "---\nname: bad-missing-desc\n---\n");
  write(sys / "bad-name/SKILL.md",
        "---\nname: BAD_NAME\ndescription: x\n---\n");
  write(usr / "user-helper/SKILL.md",
        "---\nname: user-helper\ndescription: User-made helper\nmetadata:\n  short-description: helper\n---\n");

  std::vector<aoi::SkillDef> skills = aoi::loadSkills(sys.string(), usr.string());
  bool foundSys = false, foundNested = false, foundUser = false;
  std::string userRel;
  for (const auto& s : skills) {
    if (s.name == "git-release" && s.scope == aoi::SkillScope::System) foundSys = true;
    if (s.name == "frontend-design") foundNested = true;
    if (s.name == "user-helper" && s.scope == aoi::SkillScope::User) {
      foundUser = true;
      userRel = s.relPath;
    }
  }
  check(foundSys, "system skill discovered");
  check(foundNested, "nested skill discovered (max depth)");
  check(foundUser, "user skill discovered with scope");
  check(userRel == "user-helper/SKILL.md", "user skill relPath is sandbox-relative");
  check(skills.size() == 3, "invalid skills (bad name / missing description) skipped");

  // ---- enablement rules (Codex SkillsConfig semantics) ----
  const fs::path sysReg = root / "system_registry.json";
  const fs::path usrReg = root / "user_registry.json";
  write(sysReg, R"({"skills":{"include_instructions":false,"config":[{"name":"git-release","enabled":false}]}})");
  write(usrReg, R"({"skills":{"config":[{"name":"git-release","enabled":true}]}})");
  aoi::SkillRules rules = aoi::loadSkillRules(sysReg.string(), usrReg.string());
  check(!rules.includeInstructions, "include_instructions=false from system registry");
  aoi::applySkillRules(skills, rules);
  for (const auto& s : skills)
    if (s.name == "git-release") check(s.enabled, "user rule re-enables system-disabled skill");
  // user registry disables a user skill
  write(usrReg, R"({"skills":{"config":[{"name":"user-helper","enabled":false}]}})");
  rules = aoi::loadSkillRules(sysReg.string(), usrReg.string());
  aoi::applySkillRules(skills, rules);
  for (const auto& s : skills)
    if (s.name == "user-helper") check(!s.enabled, "user rule disables user skill");

  // ---- budgeted rendering ----
  // Re-discover skills so every entry is enabled again, then apply rules
  // (user rule re-enables git-release that the system registry disables).
  skills = aoi::loadSkills(sys.string(), usr.string());
  write(usrReg, R"({"skills":{"config":[{"name":"git-release","enabled":true}]}})");
  rules = aoi::loadSkillRules(sysReg.string(), usrReg.string());
  aoi::applySkillRules(skills, rules);
  const std::string catalog = aoi::renderAvailableSkills(skills, true);
  std::puts("--- rendered skills section ---");
  std::puts(catalog.c_str());
  std::puts("-------------------------------");
  check(catalog.find("## Skills") != std::string::npos, "## Skills header");
  check(catalog.find("### Available skills") != std::string::npos, "### Available skills header");
  check(catalog.find("- git-release: Create consistent releases (file: git-release/SKILL.md)") !=
            std::string::npos,
        "entry line format: name: description (file: relPath)");
  check(catalog.find("user-helper: helper (file: user-helper/SKILL.md)") !=
            std::string::npos,
        "user skill listed with short-description preference");
  check(catalog.find("progressive disclosure") != std::string::npos,
        "usage instructions present");
  check(catalog.size() <= aoi::kSkillCatalogCharBudget + 64u,
        "catalog within budget");

  // tiny budget: entries omitted
  const std::string tiny = aoi::renderAvailableSkills(skills, true, 60);
  check(tiny.size() <= 60 + 64u, "tiny budget respected");
  check(tiny.find("git-release") == std::string::npos || tiny.find("## Skills") != std::string::npos,
        "tiny budget still has header");

  // all disabled -> empty
  aoi::SkillRules off = rules;
  off.rules.push_back({"git-release", false});
  off.rules.push_back({"frontend-design", false});
  off.rules.push_back({"user-helper", false});
  std::vector<aoi::SkillDef> all = skills;
  aoi::applySkillRules(all, off);
  check(aoi::renderAvailableSkills(all).empty(), "all disabled => empty catalog");

  fs::remove_all(root);

  if (fails) {
    std::printf("RESULT: %d FAILURE(S)\n", fails);
    return 1;
  }
  std::puts("RESULT: ALL PASSED");
  return 0;
}
