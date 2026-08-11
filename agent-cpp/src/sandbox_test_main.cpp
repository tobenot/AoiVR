// sandbox_test: standalone test for the elevated sandbox chain
// (restricted token -> CreateProcessAsUser -> helper). Exercises:
//   write into sandbox (must succeed)
//   write outside sandbox (must be denied by the OS)
//   read anywhere (must work)
#include <cstdio>
#include <string>

#include "windows_sandbox.hpp"

int main(int argc, char** argv) {
  const std::string op = argc > 1 ? argv[1] : "write-in";
  std::string reply;

  if (op == "write-in") {
    reply = aoi::sandboxExecute(
        R"json({"op":"write","path":"hello.txt","content":"hello sandbox"})json");
  } else if (op == "write-out") {
    reply = aoi::sandboxExecute(
        R"json({"op":"write","path":"C:\\Windows\\sandbox_should_fail.txt","content":"x"})json");
  } else if (op == "read") {
    reply = aoi::sandboxExecute(
        R"json({"op":"read","path":"hello.txt"})json");
  } else if (op == "bash-in") {
    reply = aoi::sandboxExecute(
        R"json({"op":"bash","command":"echo bash-ok && dir /b hello.txt"})json");
  } else if (op == "bash-out") {
    reply = aoi::sandboxExecute(
        R"json({"op":"bash","command":"echo x > C:\\Windows\\sandbox_should_fail.txt"})json");
  } else if (op == "write-wd") {
    reply = aoi::sandboxExecute(
        R"json({"op":"write","path":"D:\\workplace\\VR-AGENT\\c++-src\\agent-cpp\\build\\Release\\escape_check.txt","content":"x"})json");
  } else if (op == "agentsmd") {
    reply = aoi::sandboxExecute(
        R"json({"op":"bash","command":"echo inject > AGENTS.md 2>&1 && echo WRITTEN || echo DENIED"})json");
  } else if (op == "cwd") {
    reply = aoi::sandboxExecute(
        R"json({"op":"bash","command":"whoami && echo CD=%CD%"})json");
  } else if (op == "sql") {
    reply = aoi::sandboxExecute(
        R"json({"op":"sql_query","sql":"SELECT 1 AS x","db_path":""})json");
  } else {
    printf("usage: sandbox_test <write-in|write-out|read|bash-in|bash-out|write-wd|agentsmd|cwd|sql>\n");
    return 1;
  }
  printf("%s\n", reply.c_str());
  return 0;
}
