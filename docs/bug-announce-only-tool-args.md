# Bug 修复文档：流式工具调用参数丢失（announce-only）

> 状态：**py 与 agent 的请求已字节级完全对齐，py 成功、agent 仍 announce-only，差异收敛到「agent 进程环境」**。
> - 模型 / 网关 / 协议 / 传输 / HTTP 栈 / 请求体 / header / session / curl 选项 / 低速率守卫 / 连接复用 / cancel 机制 / 线程 → **全部排除**
> - agent 的流「正常结束」（usage 到、无错误）但工具参数为空；同一请求在 py（urllib 或 libcurl）下 4s 内拿到完整参数
> - 当前代码状态：LOW_SPEED 守卫已移除（无上限）；`curl_global_init` 已加（call_once）；流式 + 严格结算 + 自动重试；**端到端仍未通过**

## 1. Bug 现象

- 我们的客户端（C++/libcurl，`aoi-agent.exe` → `aoi_agent.dll`）走流式（`stream: true`）时，工具调用**持续性拿不到参数**（对 agent 几乎 100% 复现）：只收到首个 chunk（id + name + `arguments:""`，即 announce），后续参数 delta 永远不来。
- 表现：`read` 工具拿到空参数 → 反复空转 → 最终「网络异常，请重试」。
- 对照：Python（urllib 或 ctypes+libcurl）用**完全相同**的请求 → **从未失败**（参数完整）。
- 用户观察：「Python 脚本一直正常，但我们的客户端就不对」—— 已被实验证实。

## 2. 影响

- 工具循环无法正常工作（参数永远空）。
- 修复前的行为：空参数 → `{}` 兜底 → 工具报「缺少参数」→ 模型重试 → 再次空参数 → 死循环（浪费 token，最终失败）。
- 修复前曾因 `arguments:""` 被原样写回历史导致后续整个请求 HTTP 400/500（已修）。

## 3. 关键背景事实

- 客户端当前请求：`https://opencode.ai/zen/go/v1/chat/completions`，`thinking.enabled` + `reasoning_effort: low`，**12 个工具**，系统提示约 6275 字符（DIAG 日志的 `sys=6417` 是 hash 前缀，非长度），另带 `max_tokens: 32000`、`prompt_cache_key` + `prompt_cache_retention: 24h`（网关 prompt 前缀缓存）。
- agent 实际发送的 header（抓取自运行日志，顺序即发送顺序，共 7 个）：
  - Content-Type: application/json
  - Authorization: Bearer sk-...
  - x-opencode-session: <32hex 随机，每进程一个>
  - x-opencode-project: aoi-vr-agent
  - x-opencode-client: aoi-vr-cpp
  - x-opencode-request: aoi-vr-user
  - User-Agent: opencode/1.18.15
- agent 的 session：32 位随机 hex（`sessionId()`，每进程生成一次）；**网关按 session 绑定 body**（同一 session 换 body 会被拒 401，见 #11）。
- opencode 客户端自身架构（用户查证 opencode 源码 `packages/llm/src/protocols/`）：
  - **Responses API**（`openai-responses.ts`）：`output_item.done` 事件携带权威完整参数（the final value wins），免疫「announce 后无 delta」。
  - **Chat Completions**（`openai-chat.ts`）：**没有 done 事件**，参数只能从 delta 拼接；空 `arguments:""` chunk 被显式忽略；`finish_reason` 时 `finishAll` 统一结算，严格 JSON 解析，不完整 → `eventError`。
  - 结论：Chat Completions 路径下**协议层面无救**，谁遇到丢 delta 都会失败。

## 4. 排除的方向（按时间顺序）

| # | 假设 | 实验 | 结果 | 结论 |
|---|------|------|------|------|
| 1 | **流式解析代码有 bug** | TC-DELTA/TC-PAIR 逐 chunk 日志；多语言累加器 | 到达的 chunk 累积正常 | ❌ 排除（限定：到达 agent 的 chunk 累积正确；但参数 chunk 从未到达——见第 6 节缺口） |
| 2 | **「客户端差异」是真实差异** | 累加器重测 | 之前差异是我正则只抓 announce chunk 的**测量伪影** | ❌ 排除：伪影 |
| 3 | **非流式 vs 流式** | `stream: false` | 非流式完全正常（端到端通过） | ✅ 有效缓解；但**用户要求流式**，非流式只是绕开 |
| 4 | **模型特有缺陷** | model_test.py 测 deepseek-v4-flash / mimo-v2.5 | Python 下 6/6 参数完整 | ❌ 排除：与模型无关 |
| 5 | **网关随机间歇性丢 delta** | agent 3/3 重试全失败；Python 同时间段从不失败 | 对 agent 持续性、对 Python 从未 | ❌ 排除：不是随机 |
| 6 | **粘性路由钉到坏节点** | 重试时换 session key 后缀 | 3/3 仍 announce-only | ❌ 排除（且换 session 破坏缓存，已删除） |
| 7 | **请求体特征**（工具数/系统提示/thinking/effort/usage/头） | bisect_test.py 拉满全部特征 | 4/4 正常 | ❌ 排除（在 Python/urllib 下） |
| 8 | **HTTP 版本差异**（HTTP/2 vs 1.1） | 用户确认之前测过 | 无差异 | ❌ 排除 |
| 9 | **HTTP 客户端栈**（libcurl vs urllib） | `http-bisect/` 三路对照：py+urllib / cpp+libcurl 独立 exe / py+ctypes 加载 libcurl.dll，同一逻辑 | 三路全部正常 | ❌ 排除（libcurl 基础配置下） |
| 10 | **agent 的 curl 选项**（NOPROGRESS=0+XFERINFO、CONNECTTIMEOUT=15、LOW_SPEED_LIMIT=1、LOW_SPEED_TIME=45、NOSIGNAL、FOLLOWLOCATION、MAXREDIRS=5） | py_libcurl 复刻全部选项 | LOW_SPEED_TIME=45 时 2/4 超时（网关停顿 >45s 被主动断开）；300s 时成功率上升 | ⚠️ 守卫确实会误杀停顿流（已移除），但**不是唯一根因**（见 #13） |
| 11 | **请求体/session/header 全量对齐** | py 用 agent 原始字节 body（req_replay_tN.json）+ 对齐 header + 32hex session | 固定假 session（用过 dummy body）→ **401**；**全新随机 session** → urllib 2/2 成功、py_libcurl 4s 拿到完整参数 | ❌ 排除：请求体/header/session 全部无关。**附带真相：网关按 session 绑定 body**，同一 session 换 body 会被拒 401——早期 replay 401 的原因 |
| 12 | **header 顺序 / 字节级对齐** | 抓取 agent 实际 header（7 个，User-Agent 在最后）→ py_libcurl 按同顺序发送 | py_libcurl **4s 成功**（参数完整） | ❌ 排除：header 顺序无关（作为「全量对齐」里程碑） |
| 13 | **LOW_SPEED 守卫（移除后验证）** | 移除守卫 + 加 curl_global_init 后重跑 agent | 仍 3/3 announce-only | ❌ 排除：守卫不是根因（保留移除，停顿确实常见） |
| 14 | **cancel 机制** | 读代码：agent 的 cancel = `!running_`（仅 agent 停止时触发） | 运行中永不取消 | ❌ 排除 |
| 15 | **连接复用（持久 handle）** | py_libcurl `--reuse`（同 handle + curl_easy_reset 复用连接池）连发 4 次 | 2/4 成功（参数完整）、2/4 300s 超时，**无 announce-only** | ❌ 排除 |
| 16 | **线程** | 分析：ctypes 调 native curl 释放 GIL；agent 的请求在其线程内同步 perform，无并发差异；agent 已补 curl_global_init（call_once） | 无证据支持 | ❌ 排除（弱证据，未专门实验） |

**排除后的剩余差异（agent 独有）**：
- agent 的流「**正常结束**」（postStream 返回 OK、usage 打印、无错误码）但工具参数空 —— 服务器对 agent 的连接**关闭了流但没发参数 delta**；py 的连接则完整发出。
- 尚未验证的唯一大项：**agent 原始响应字节**（参数 delta 是否真的不在 agent 收到的字节里——需临时抓流确认「服务器没发」还是「解析层丢失」）。

## 5. 当前状态与代码

- `Config::stream = true`（流式默认，按用户要求「禁止非流式」）。
- 流式结算为 **opencode 风格**：空 `arguments:""` 忽略、按 index 累积、流结束严格 JSON 校验、参数丢失自动重试（最多 3 次）、重试耗尽返回失败且绝不把空参数写回历史。
- `LOW_SPEED_LIMIT/TIME` **已移除**（无上限；网关停顿 45s～>300s 是常态，守卫会误杀）。
- `curl_global_init` **已加**（`std::call_once` 线程安全，对齐 py 的全局初始化）。
- 删除：旧 `{}` 兜底；换 session 重试；所有 TEMP DEBUG 打印。
- 实验工具 `http-bisect/`：`py_client.py`（urllib）、`cpp_client.cpp`（libcurl 独立 exe）、`py_libcurl_client.py`（Python ctypes 加载 libcurl.dll；`--replay` 重放 agent 真实请求体；`--reuse` 同 handle 复用；`--incremental` 复刻 agent 回调内增量解析；`--thread` 预留）。共享构建：`-DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_CURL=OFF` → `libcurl.dll`。
- 残留 TEMP：`req_replay_tN.json` / `hdr_replay.txt` 抓取代码（`llm_client.cpp` 流式分支内，诊断完必删）。

## 6. 未解问题 / 下一步

- **抓 agent 原始响应字节**（最高优先，唯一剩余证据缺口）：临时在 llm_client 保存流式原始字节（stream_debug），跑一次 agent，确认：
  - 字节里有参数 delta → 解析/缓冲层丢失（但 TC 日志和 py 复刻增量解析都正常，需再深挖）
  - 字节里没有参数 delta → 服务器没发 → 差异在连接层面（agent 的连接状态 vs py 的连接）
- 若确认「服务器没发」：对比 agent 与 py 的连接细节（TCP 复用、HTTP/1.1 keep-alive 状态、TLS session 复用），或尝试 agent 每请求新建连接（`CURLOPT_FRESH_CONNECT`）验证。
- 备选兜底：保持流式，把「检测到参数丢失」升级为**切非流式补一次**（流式失败后自动降级一次性 JSON 重试）。
