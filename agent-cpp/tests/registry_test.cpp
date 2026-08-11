// Registry unit test: parse / merge system+user registries and render the
// skill prompt injection. Standalone (no hardware, no network).
#include <cstdio>
#include <fstream>
#include <string>

#include "registry.hpp"

namespace {

const std::string kSys = "registry_test_system.json";
const std::string kUsr = "registry_test_user.json";

void write(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  f << content;
}

int fails = 0;

void check(bool cond, const char* what) {
  std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) ++fails;
}

} // namespace

int main() {
  write(kSys, R"({
    "version": 1,
    "tools": [{"name":"read"},{"name":"bash"},{"name":"write"},{"name":"hook_manage"}]
  })");
  write(kUsr, R"({
    "version": 1,
    "tools": [{"name":"bash","enabled":false},{"name":"write","description":"自定义覆盖"}]
  })");

  const aoi::ToolRegistry reg = aoi::loadRegistries(kSys, kUsr);

  // merge: user disable wins over system
  check(!reg.toolEnabled("bash"), "bash disabled by user registry");
  // untouched system tool stays enabled
  check(reg.toolEnabled("read"), "read enabled from system registry");
  // description override
  const aoi::ToolEntry* w = reg.findTool("write");
  check(w != nullptr && w->hasDescription && w->description == "自定义覆盖",
        "write description overridden by user registry");
  check(w != nullptr && w->enabled, "write still enabled");

  // empty registries: everything enabled
  write(kSys, "{}");
  write(kUsr, "{}");
  const aoi::ToolRegistry empty = aoi::loadRegistries(kSys, kUsr);
  check(empty.toolEnabled("bash"), "missing registry => tool enabled");

  std::remove(kSys.c_str());
  std::remove(kUsr.c_str());

  if (fails) {
    std::printf("RESULT: %d FAILURE(S)\n", fails);
    return 1;
  }
  std::puts("RESULT: ALL PASSED");
  return 0;
}
