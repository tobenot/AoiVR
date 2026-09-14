#include "hook_tools.hpp"

#include "hook_scheduler.hpp"

namespace aoi {

ToolDefinition makeHookManageTool(HookScheduler* scheduler) {
  ToolDefinition t;
  t.name = "hook_manage";
  t.label = "hook_manage";
  t.description =
      "注册/查询/取消定时 hook。到点触发 LLM 编排或执行沙箱脚本。通用调度工具，不绑定任何业务。"
      "actions: create（注册新 hook）、list（列出全部）、cancel（删除）、pause/resume（暂停/恢复）。"
      "trigger.type: interval（interval_seconds>=60）或 at（HH:MM）。"
      "action_type: llm（到点独立 LLM 推理）、script（沙箱脚本，结果交由 LLM 判断是否提醒）、"
      "llm_script（LLM 推理中自行调用脚本工具）。"
      "script_path 必须是沙箱空间内相对路径（禁止绝对路径、..、盘符）。";
  t.parameters = {
      {"type", "object"},
      {"properties",
       nlohmann::json{
           {"action",
            {{"type", "string"},
             {"enum", nlohmann::json::array({"create", "list", "cancel", "pause", "resume"})},
             {"description", "要执行的操作"}}},
           {"name",
            {{"type", "string"},
             {"description", "hook 名称；cancel/pause/resume 用 id 或 name 定位"}}},
           {"trigger",
            {{"type", "object"},
             {"properties",
              nlohmann::json{{"type", {{"enum", nlohmann::json::array({"interval", "at"})},
                                       {"description", "触发方式"}}},
                             {"interval_seconds",
                              {{"type", "integer"}, {"minimum", 60}}},
                             {"at", {{"type", "string"},
                                     {"description", "HH:MM 格式的每日时刻"}}}}},
             {"description", "触发规则"}}},
           {"action_type",
            {{"type", "string"},
             {"enum", nlohmann::json::array({"llm", "script", "llm_script"})},
             {"description", "到点后执行什么"}}},
           {"intent",
            {{"type", "string"},
             {"description", "触发时注入给 LLM 的意图描述（自由文本）"}}},
           {"script_path",
            {{"type", "string"},
             {"description", "沙箱空间内相对路径（script/llm_script 用），禁止绝对路径/.."}}},
           {"context",
            {{"type", "object"},
             {"description", "触发时附加数据（如游标）"}}}}},
      {"required", nlohmann::json::array({"action"})},
  };
  t.execute = [scheduler](const std::string&, const nlohmann::json& args) -> nlohmann::json {
    if (!scheduler) {
      return nlohmann::json{{"content", "(hook_manage unavailable: scheduler not running)"}};
    }
    return scheduler->manage(args);
  };
  return t;
}

} // namespace aoi
