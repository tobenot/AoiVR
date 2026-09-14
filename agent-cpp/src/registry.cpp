#include "registry.hpp"

#include <fstream>

namespace aoi {

namespace {

std::string get(const nlohmann::json& obj, const char* key) {
  if (obj.is_object() && obj.contains(key) && obj[key].is_string()) {
    return obj[key].get<std::string>();
  }
  return "";
}

ToolEntry parseTool(const nlohmann::json& j) {
  ToolEntry e;
  e.name = get(j, "name");
  if (j.is_object() && j.contains("enabled")) {
    e.enabledSet = true;
    if (j["enabled"].is_boolean()) e.enabled = j["enabled"].get<bool>();
    else if (j["enabled"].is_string()) {
      const std::string v = j["enabled"].get<std::string>();
      e.enabled = !(v == "false" || v == "0" || v == "off");
    }
  }
  e.hasLabel = j.is_object() && j.contains("label") && j["label"].is_string();
  if (e.hasLabel) e.label = j["label"].get<std::string>();
  e.hasDescription = j.is_object() && j.contains("description") &&
                     j["description"].is_string();
  if (e.hasDescription) e.description = j["description"].get<std::string>();
  e.hasParameters = j.is_object() && j.contains("parameters") &&
                    j["parameters"].is_object();
  if (e.hasParameters) e.parameters = j["parameters"];
  return e;
}

} // namespace

const ToolEntry* ToolRegistry::findTool(const std::string& name) const {
  for (const auto& t : tools) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

bool ToolRegistry::toolEnabled(const std::string& name) const {
  const ToolEntry* e = findTool(name);
  return e == nullptr || e->enabled;
}

ToolRegistry loadRegistryFile(const std::string& path) {
  ToolRegistry reg;
  std::ifstream f(path);
  if (!f.is_open()) return reg;
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(f);
  } catch (...) {
    return reg;
  }
  if (!root.is_object()) return reg;
  if (root.contains("tools") && root["tools"].is_array()) {
    for (const auto& j : root["tools"]) {
      if (j.is_object()) reg.tools.push_back(parseTool(j));
    }
  }
  return reg;
}

ToolRegistry loadRegistries(const std::string& systemPath,
                            const std::string& userPath) {
  const ToolRegistry sys = loadRegistryFile(systemPath);
  const ToolRegistry usr = loadRegistryFile(userPath);

  ToolRegistry merged = sys;
  if (!usr.tools.empty()) {
    for (const auto& u : usr.tools) {
      if (u.name.empty()) continue;
      bool overrode = false;
      for (auto& t : merged.tools) {
        if (t.name == u.name) {
          if (u.enabledSet) {
            t.enabled = u.enabled;
            t.enabledSet = true;
          }
          if (u.hasLabel) t.label = u.label, t.hasLabel = true;
          if (u.hasDescription) t.description = u.description, t.hasDescription = true;
          if (u.hasParameters) t.parameters = u.parameters, t.hasParameters = true;
          overrode = true;
          break;
        }
      }
      if (!overrode) merged.tools.push_back(u);
    }
  }
  return merged;
}

void ensureDefaultRegistries(const std::string& systemPath,
                             const std::string& userPath) {
  {
    std::ifstream f(systemPath);
    if (!f.is_open()) {
      nlohmann::json tools = nlohmann::json::array();
      for (const char* name : {"read",   "bash",         "edit",
                               "write",  "sql_query",    "convert",
                               "screenshot", "system",   "interpretation",
                               "awareness", "context",   "vr_set_brightness",
                               "hook_manage"}) {
        tools.push_back(nlohmann::json{{"name", name}});
      }
      const nlohmann::json sys = {
          {"version", 1},
          {"comment",
           "系统注册表：由 agent 控制，沙箱用户只读。工具改动后重启 agent 生效。"
           "sandbox\\user_registry.json 中的同名条目可覆盖。"},
          {"tools", tools},
          {"skills",
           {{"comment",
             "技能启停规则（Codex SkillsConfig 语义）：config 按 name 匹配，"
             "enabled:false 可禁用同名 SKILL.md 技能。技能本体是 SKILL.md 文件："
             "系统技能在 sandbox\\skills\\<name>\\SKILL.md（只读），用户技能在 "
             "sandbox\\skills_user\\<name>\\SKILL.md（沙箱内可编辑）。"},
            {"include_instructions", true},
            {"config", nlohmann::json::array()}}},
      };
      std::ofstream out(systemPath);
      if (out) out << sys.dump(2) << "\n";
    }
  }
  {
    std::ifstream f(userPath);
    if (!f.is_open()) {
      const nlohmann::json usr = {
          {"version", 1},
          {"comment",
           "用户注册表：沙箱内可用 write 工具编辑（模型或用户均可改）。"
           "enabled:false 可禁用系统工具/技能；用户技能直接创建 "
           "sandbox\\skills_user\\<name>\\SKILL.md（frontmatter 含 name+description）。"
           "技能与工具改动需重启 agent。"},
          {"tools", nlohmann::json::array()},
          {"skills",
           {{"include_instructions", true},
            {"config", nlohmann::json::array()}}},
      };
      std::ofstream out(userPath);
      if (out) out << usr.dump(2) << "\n";
    }
  }
}

} // namespace aoi
