#include "skills.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace aoi {

namespace {

namespace fs = std::filesystem;

// Codex loader/mod.rs MAX_SCAN_DEPTH.
constexpr int kMaxScanDepth = 6;

std::string sanitizeSingleLine(const std::string& raw) {
  std::string out;
  bool inSpace = false;
  for (const char c : raw) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!out.empty()) inSpace = true;
    } else {
      if (inSpace && !out.empty()) out += ' ';
      inSpace = false;
      out += c;
    }
  }
  return out;
}

// Extract the `---`-delimited frontmatter block (Codex extract_frontmatter).
// The closing line must be EXACTLY "---" (trailing whitespace allowed) - a
// bare "---" inside the markdown body (a common horizontal rule) must not end
// the frontmatter early.
bool extractFrontmatter(const std::string& contents, std::string& out) {
  const auto isDelim = [](const std::string& line) {
    size_t s = 0;
    while (s < line.size() &&
           (line[s] == ' ' || line[s] == '\t' || line[s] == '\r'))
      ++s;
    size_t e = line.size();
    while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r'))
      --e;
    return e - s == 3 && line.compare(s, 3, "---") == 0;
  };
  size_t pos = 0;
  while (pos < contents.size()) {
    size_t lineEnd = contents.find('\n', pos);
    if (lineEnd == std::string::npos) lineEnd = contents.size();
    const std::string line = contents.substr(pos, lineEnd - pos);
    if (isDelim(line)) break;
    if (!line.empty() && line.find_first_not_of(" \t\r") != std::string::npos)
      return false;  // first non-empty line must be the opening delimiter
    pos = lineEnd + 1;
  }
  if (pos >= contents.size()) return false;
  size_t start = contents.find('\n', pos);
  if (start == std::string::npos) return false;
  ++start;
  // Find the closing "---" line.
  size_t cur = start;
  while (cur < contents.size()) {
    size_t lineEnd = contents.find('\n', cur);
    if (lineEnd == std::string::npos) lineEnd = contents.size();
    const std::string line = contents.substr(cur, lineEnd - cur);
    if (isDelim(line)) {
      out = contents.substr(start, cur > start ? cur - start : 0);
      // strip one trailing newline before the closing delimiter
      if (!out.empty() && out.back() == '\n') out.pop_back();
      return !out.empty();
    }
    cur = lineEnd + 1;
  }
  return false;
}

// Parse a single `key: value` line (line-oriented YAML subset, sufficient for
// the recognized fields: name, description, metadata.short-description).
// Continuation lines (indented, no key) are folded with a space, matching
// YAML plain-scalar folding for the common two-line description case.
std::string yamlValue(const std::string& frontmatter, const std::string& key) {
  const std::string prefix = key + ":";
  const auto isKeyLine = [](const std::string& line) {
    size_t s = 0;
    while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
    const size_t colon = line.find(':', s);
    if (colon == std::string::npos) return false;
    std::string k = line.substr(s, colon - s);
    for (const char c : k) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
        return false;
    }
    return !k.empty();
  };
  size_t pos = 0;
  while (pos < frontmatter.size()) {
    size_t lineEnd = frontmatter.find('\n', pos);
    if (lineEnd == std::string::npos) lineEnd = frontmatter.size();
    std::string line = frontmatter.substr(pos, lineEnd - pos);
    size_t start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t'))
      ++start;
    if (line.compare(start, prefix.size(), prefix) == 0) {
      std::string value = line.substr(start + prefix.size());
      size_t v = 0;
      while (v < value.size() && std::isspace(static_cast<unsigned char>(value[v])))
        ++v;
      value = value.substr(v);
      // Fold indented continuation lines (YAML plain-scalar folding).
      size_t next = lineEnd + 1;
      while (next < frontmatter.size()) {
        size_t nextEnd = frontmatter.find('\n', next);
        if (nextEnd == std::string::npos) nextEnd = frontmatter.size();
        const std::string nl = frontmatter.substr(next, nextEnd - next);
        const bool indented =
            !nl.empty() && (nl.front() == ' ' || nl.front() == '\t');
        if (!indented || isKeyLine(nl)) break;
        const size_t nv = nl.find_first_not_of(" \t");
        if (nv == std::string::npos) { next = nextEnd + 1; continue; }
        if (!value.empty()) value += ' ';
        value += nl.substr(nv);
        next = nextEnd + 1;
      }
      // strip an inline comment (" # ...")
      for (size_t i = 0; i + 1 < value.size(); ++i) {
        if (value[i] == ' ' && value[i + 1] == '#') {
          value = value.substr(0, i);
          break;
        }
      }
      // strip surrounding quotes
      if (value.size() >= 2 &&
          ((value.front() == '"' && value.back() == '"') ||
           (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
      }
      return sanitizeSingleLine(value);
    }
    pos = lineEnd + 1;
  }
  return "";
}

// Longest common parent directory prefix of a SKILL.md path and its root.
std::string relativeToRoot(const fs::path& root, const fs::path& abs) {
  std::error_code ec;
  fs::path rel = fs::relative(abs, root, ec);
  if (ec) return "";  // caller skips entries with an empty relPath
  std::string out;
  for (const auto& part : rel) {
    if (!out.empty()) out += "/";
    out += part.string();
  }
  return out;
}

// Maximum SKILL.md size loaded into memory (a stray multi-GB file must not
// OOM the agent on scan).
constexpr size_t kSkillMaxBytes = 1 * 1024 * 1024;

// Truncate a UTF-8 string to at most maxBytes bytes, never splitting a
// multi-byte sequence (falls back to the previous character boundary).
std::string utf8Truncate(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
    --end;  // back up over continuation bytes
  return s.substr(0, end);
}

} // namespace

bool isValidSkillName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  if (name.front() == '-' || name.back() == '-') return false;
  bool prevHyphen = false;
  for (const char c : name) {
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    const bool hyphen = c == '-';
    if (!lower && !digit && !hyphen) return false;
    if (hyphen) {
      if (prevHyphen) return false;  // no consecutive --
      prevHyphen = true;
    } else {
      prevHyphen = false;
    }
  }
  return true;
}

bool parseSkillFrontmatter(const std::string& contents,
                           const std::string& defaultName,
                           ParsedSkillFrontmatter& out) {
  std::string fm;
  if (!extractFrontmatter(contents, fm)) return false;
  std::string name = yamlValue(fm, "name");
  if (name.empty()) name = defaultName;
  std::string description = yamlValue(fm, "description");
  if (description.empty()) return false;
  out.name = sanitizeSingleLine(name);
  out.description = sanitizeSingleLine(description);
  out.shortDescription = sanitizeSingleLine(yamlValue(fm, "short-description"));
  if (out.description.empty()) return false;
  return true;
}

std::vector<SkillDef> scanSkillsDir(const std::string& root, int maxDepth,
                                    SkillScope scope) {
  std::vector<SkillDef> out;
  if (root.empty()) return out;
  fs::path rootPath(root);
  std::error_code ec;
  if (!fs::is_directory(rootPath, ec)) return out;

  struct Ctx {
    fs::path dir;
    int depth;
  };
  std::vector<Ctx> stack{{rootPath, 0}};
  while (!stack.empty()) {
    const Ctx ctx = stack.back();
    stack.pop_back();
    for (fs::directory_iterator it(ctx.dir, ec), end; it != end && !ec;
         it.increment(ec)) {
      const fs::path p = it->path();
      if (it->is_directory(ec) && ctx.depth < maxDepth) {
        stack.push_back({p, ctx.depth + 1});
        continue;
      }
      if (it->is_regular_file(ec) && p.filename() == "SKILL.md") {
        // Bound the file size before reading the whole thing into memory.
        std::error_code sizeEc;
        const uintmax_t size = it->file_size(sizeEc);
        if (sizeEc || size > kSkillMaxBytes) continue;
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        std::string contents((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        const std::string defaultName = p.parent_path().filename().string();
        ParsedSkillFrontmatter parsed;
        if (!parseSkillFrontmatter(contents, defaultName, parsed)) continue;
        if (!isValidSkillName(parsed.name)) continue;
        SkillDef def;
        def.name = parsed.name;
        def.description = parsed.description;
        def.shortDescription = parsed.shortDescription;
        def.relPath = relativeToRoot(rootPath, p);
        if (def.relPath.empty()) continue;  // fs::relative failed
        def.absPath = p.string();
        def.scope = scope;
        out.push_back(std::move(def));
      }
    }
  }
  return out;
}

std::vector<SkillDef> loadSkills(const std::string& systemDir,
                                 const std::string& userDir, int maxDepth) {
  std::vector<SkillDef> skills = scanSkillsDir(systemDir, maxDepth, SkillScope::System);
  std::vector<SkillDef> user = scanSkillsDir(userDir, maxDepth, SkillScope::User);
  skills.insert(skills.end(), user.begin(), user.end());
  return skills;
}

SkillRules loadSkillRules(const std::string& systemRegistryPath,
                          const std::string& userRegistryPath) {
  SkillRules rules;
  const auto parseOne = [&rules](const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    nlohmann::json root;
    try {
      root = nlohmann::json::parse(f);
    } catch (...) {
      return;
    }
    if (!root.is_object() || !root.contains("skills")) return;
    const auto& skills = root["skills"];
    if (!skills.is_object()) return;
    if (skills.contains("include_instructions") &&
        skills["include_instructions"].is_boolean()) {
      rules.includeInstructions = skills["include_instructions"].get<bool>();
    }
    if (skills.contains("config") && skills["config"].is_array()) {
      for (const auto& j : skills["config"]) {
        if (!j.is_object() || !j.contains("enabled") ||
            !j["enabled"].is_boolean())
          continue;
        SkillRule r;
        r.enabled = j["enabled"].get<bool>();
        if (j.contains("name") && j["name"].is_string()) {
          r.name = j["name"].get<std::string>();
        } else if (j.contains("path") && j["path"].is_string()) {
          r.name = j["path"].get<std::string>();  // name-based matching only
        } else {
          continue;
        }
        rules.rules.push_back(std::move(r));
      }
    }
  };
  parseOne(systemRegistryPath);
  parseOne(userRegistryPath);  // user rules appended -> override system
  return rules;
}

void applySkillRules(std::vector<SkillDef>& skills, const SkillRules& rules) {
  for (const auto& rule : rules.rules) {
    for (auto& s : skills) {
      if (s.name == rule.name) s.enabled = rule.enabled;
    }
  }
}

std::string renderAvailableSkills(const std::vector<SkillDef>& skills,
                                  bool includeInstructions, int budget) {
  std::vector<const SkillDef*> enabled;
  for (const auto& s : skills) {
    if (s.enabled) enabled.push_back(&s);
  }
  if (enabled.empty()) return "";

  const std::string intro =
      "A skill is a set of instructions provided through a `SKILL.md` source. "
      "Below is the list of skills that can be used. Each entry includes a "
      "name, description, and the file path of its `SKILL.md`.";
  const std::string usage =
      "- Trigger rules: If the user names a skill or the task clearly matches "
      "a skill's description, use that skill for that turn.\n"
      "- How to use (progressive disclosure): after deciding to use a skill, "
      "read its `SKILL.md` completely (with the read tool) before taking task "
      "actions. When `SKILL.md` references relative files (scripts/, "
      "references/, assets/), resolve them relative to its directory; prefer "
      "running or patching provided scripts instead of retyping large code "
      "blocks.\n"
      "- Context hygiene: do not load unrelated references or carry skill "
      "instructions across turns unless re-mentioned.";

  // - name: description (file: relPath)
  const auto renderLine = [](const SkillDef& s, const std::string& desc) {
    std::string line = "- " + s.name;
    if (!desc.empty()) line += ": " + desc;
    line += " (file: " + s.relPath + ")";
    return line;
  };

  std::string head = "## Skills\n" + intro;
  if (includeInstructions) head += "\n" + usage;
  head += "\n### Available skills\n";

  // Entry descriptions: truncate to the per-entry cap on a UTF-8 boundary.
  std::vector<std::string> descs;
  descs.reserve(enabled.size());
  for (const auto* s : enabled) {
    std::string d = s->shortDescription.empty() ? s->description
                                                : s->shortDescription;
    if (static_cast<int>(d.size()) > kSkillDescriptionMaxChars) {
      d = utf8Truncate(d, static_cast<size_t>(kSkillDescriptionMaxChars)) + "...";
    }
    descs.push_back(std::move(d));
  }

  // Level 1: everything fits -> full descriptions.
  {
    std::string body;
    for (size_t i = 0; i < enabled.size(); ++i)
      body += renderLine(*enabled[i], descs[i]) + "\n";
    if (static_cast<int>(head.size() + body.size()) <= budget) {
      return head + body;
    }
  }

  // Level 2: name-only lines fit -> distribute description chars
  // round-robin across the remaining budget.
  {
    std::string minBody;
    for (size_t i = 0; i < enabled.size(); ++i)
      minBody += renderLine(*enabled[i], "") + "\n";
    const int base = static_cast<int>(head.size() + minBody.size());
    if (base <= budget) {
      std::vector<size_t> alloc(enabled.size(), 0);
      int remaining = budget - base;
      bool changed = true;
      while (changed && remaining > 0) {
        changed = false;
        for (size_t i = 0; i < enabled.size() && remaining > 0; ++i) {
          if (alloc[i] >= descs[i].size()) continue;
          // cost of adding one more char (line grows by 1)
          ++alloc[i];
          --remaining;
          changed = true;
        }
      }
      std::string body;
      for (size_t i = 0; i < enabled.size(); ++i)
        body += renderLine(*enabled[i], utf8Truncate(descs[i], alloc[i])) + "\n";
      return head + body;
    }
  }

  // Level 3: omit trailing entries that do not fit.
  {
    std::string body;
    int used = 0;
    for (size_t i = 0; i < enabled.size(); ++i) {
      const std::string line = renderLine(*enabled[i], "") + "\n";
      if (used + static_cast<int>(line.size()) > budget) break;
      used += static_cast<int>(line.size());
      body += line;
    }
    if (body.empty()) return "";
    return head + body;
  }
}

void ensureSkillDirs(const std::string& systemDir, const std::string& userDir) {
  std::error_code ec;
  if (!systemDir.empty()) fs::create_directories(systemDir, ec);
  ec.clear();
  if (!userDir.empty()) fs::create_directories(userDir, ec);
}

} // namespace aoi
