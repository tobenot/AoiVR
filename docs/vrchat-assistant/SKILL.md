---
name: vrchat-assistant
description: VRChat 集成操作技能。当用户提到 VRChat / VRCX / 世界 / 地图 / 房间 / 模型 / 头像 / 好友 / 通知 / 推荐房间 / 传送 / 加入世界 / 换模型 等任何 VRChat 相关需求时使用，即使没有明说 "VRChat"（例如 "帮我找个恐怖房间"、"我朋友在哪"、"推荐个新地图"）。本技能提供完整操作知识：从 VRCX 数据库提取登录会话、调用 VRChat 官方 API（搜索/推荐/过滤/排序世界与模型、查询好友与通知、传送与换装）、以及深链启动游戏。配合 sql_query / convert / fetch 工具使用，让 AI 自主完成从取凭据到最终操作的完整链路。铁律：**好友在线状态、当前位置、世界热度、当前通知等实时/非历史/非累积数据，必须先调 VRChat API 现查，严禁凭记忆、猜答案或拿旧数据作答**。
---

# VRChat 助手（vrchat-assistant）

让 AI 自主完成 VRChat 相关操作：**读 VRCX 数据库拿会话 → 调 VRChat API → 按需过滤排序 → 呈现结果 / 启动游戏**。

本技能是知识型技能——它不新增工具，而是教会你使用现有的 `sql_query` / `convert` / `fetch`（以及本地 `bash`/python）工具完成 VRChat 操作。先读本文件，需要端点/表结构细节时再按索引读 `references/` 下的文件。

## ⚠️ 铁律：实时数据必须先查 API（违反 = 答错）

**在线好友、用户当前位置、世界/房间当前热度与在线人数、待处理通知、当前头像——这些数据是"瞬间状态"，不会记在本地，也绝不可凭印象编造。** 每次被问到时都必须先调用 VRChat API 现查：

- "我朋友在哪 / 谁在线" → `GET /auth/user/friends`（实时，**不能**用记忆或旧 Feed 推断）
- "这个房间多少人 / 哪个世界最火" → `GET /worlds`（实时 heat/popularity/online）
- "有没有新通知" → `GET /auth/user/notifications`（当前时刻）
- 任何"现在/当前/目前/正在"的问题，默认都要先查 API

只有**历史/累积数据**（游戏日志、好友动态 Feed 时间线、活动热力图、备注、访问历史）才用 VRCX 本地数据库。

## 能力边界

能做的事：
- 搜索/推荐世界（关键词、热度、时间过滤、排除已访问）
- 加入世界（生成启动链接并唤起 VRChat）
- 搜索模型、更换模型
- **第三方头像仓库反查**（看到/好友在用的模型 → 查来源与公开状态 → 收藏或换装，见工作流 F 与 `references/avatar-provider.md`）
- 查询好友（在线状态、位置）、用户信息、通知
- 查询 VRCX 本地数据（游戏日志、好友动态 Feed、活动热力图、备注、收藏、通知历史）

注意：
- **VRChat API 是第三方非公开接口**，行为可能随时变化；以 `references/vrchat-api.md` 为准，出错时以实际响应为准。
- API 有**限流**（每分钟请求数、429 退避），搜索/列表请求保持克制，善用本地库缓存。
- 写操作（换模型、传送、接受通知）前**先向用户确认**；只读操作（查询、搜索）可直接执行。
- 本技能是**长流程任务**：从取会话、读库、多轮 API 调用到最终输出，整体可能耗时**数分钟**。**动手前先向用户说明预期**（例如："正在执行 VRChat 相关操作，可能需要几分钟，请稍候"），再继续执行。
- 本技能的操作对象是用户自己的账号（经 VRCX 授权），cookie 属敏感凭据：**绝不在回复或文件中输出完整 cookie 值**（可显示末 6 位）。

## 前置条件

- 用户机器装有 VRCX（开源 VRChat 辅助工具）且**已登录**（VRChat 账号会话存在）。
- 数据库默认位置：`%APPDATA%\VRCX\VRCX.sqlite3`（实际文件名是 VRCX.sqlite3；可通过 `%APPDATA%\VRCX\VRCX.json` 的 `VRCX_DatabaseLocation` 自定义目录，注意 JSON 里反斜杠是双写的）。Aoi agent 的 `sql_query` 工具会自动使用该默认路径或 `aoi_config.json` 的 `vrcxDbPath`。
- 若 VRCX 未登录/无 cookie：告诉用户先打开 VRCX 登录一次，不要编造数据。

## 认证：三步拿到可用会话

VRChat API 需要两个东西：`apiKey`（公开，无需登录）和 `auth` cookie（用户会话）。**所有网络请求一律用 `fetch` 工具**（沙箱 bash 无法做 TLS，curl 会以错误 35 失败）：

### 第 1 步：取 apiKey（公开，无需登录）

```
fetch(url="https://api.vrchat.cloud/api/1/config",
      headers=["User-Agent: AoiVR/0.1.0 (https://github.com/keybodhi/AoiVR)"])
```
响应 JSON 中的 API 密钥字段**以实际响应为准**：旧版本叫 `apiKey`，当前版本已更名为 **`clientApiKey`**（读取时两种都试）。该密钥当前多数端点已不强制，但携带兼容性最好（`?apiKey=<值>` 或 `?clientApiKey=<值>`）。

### 第 2 步：从 VRCX 数据库提取 auth cookie

VRCX 的 `cookies` 表只有一行（`key='default'`），`value` 是 **Base64 编码的 JSON 数组**（.NET `System.Net.Cookie` 序列化，字段为大写开头：`Name`/`Value`/`Domain`/`Path`/`Expires`/`HttpOnly`/`Secure`/`TimeStamp`）。其中 `Name == "auth"` 且 `Domain` 含 `vrchat.cloud` 的 cookie 就是 API 会话凭据。

**推荐用 python 一步完成**（最可靠，Windows 自带 python 或 `py`）：

```bash
python -c "import sqlite3,base64,json,os; p=os.path.expandvars(r'%APPDATA%\VRCX\VRCX.sqlite3'); db=sqlite3.connect(f'file:{p}?mode=ro',uri=True); b64=db.execute(\"SELECT value FROM cookies WHERE key='default'\").fetchone()[0]; cs=json.loads(base64.b64decode(b64)); auth=[c['Value'] for c in cs if c.get('Name')=='auth' and 'vrchat' in c.get('Domain','')]; print(auth[0] if auth else 'NO_AUTH')"
```

输出即 auth token。把值保存到 shell 变量中复用（例如写入 `%TEMP%\vrchat_auth.txt` 或直接在后续命令里引用），但**不要**打印完整值。

如果项目是 Aoi agent 环境且有 `sql_query`/`convert` 工具，等价流程：`sql_query` 查 `cookies` 表 → `convert` 的 `base64_decode` 解码 → `json_query` 按 `0.Value` 提取（先 `json_query` 空路径看结构）。用哪种方式取决于当前环境的工具集。

### 第 3 步：带凭据调用 API

```
fetch(url="https://api.vrchat.cloud/api/1/auth/user?apiKey=<key>",
      headers=["Cookie: auth=<token>",
               "User-Agent: AoiVR/0.1.0 (https://github.com/keybodhi/AoiVR)",
               "Accept: application/json"])
```

- **User-Agent 必须带联系方式**（项目名 + 仓库/邮箱），否则返回 401。
- 响应含 `error` 字段即失败；`401` 通常表示 cookie 失效（让用户重新登录 VRCX）。
- 大响应（世界/好友列表）：`fetch` 超限不会丢数据——完整响应自动保存到沙箱 workspace 的 `out\` 目录（返回里带文件路径），用 `read` 工具的 `offset` 参数分页读取；也可以直接把 `max_bytes` 调大（如 200000）一次拿全。

### 认证过期处理

- 请求返回 `401` / `Invalid Credentials` / `requiresTwoFactorAuth`，或 VRCX 数据库不存在、`cookies` 表为空 → **会话已失效或从未登录**。
- 处理方式：**提示用户"VRChat 会话已过期（或未登录），请打开 VRCX 重新登录一次，然后告诉我"**，然后停止尝试。
- 不要反复重试同一请求；**不要尝试用账号密码直接登录 API**（每次密码认证都消耗 session 配额，Session Limit 触发会被临时封禁）。
- 用户重新登录 VRCX 后，重新走第 2 步提取新 cookie 即可。

## 数据源选择：API 优先，本地库兜底

实时数据一律走 **API**（用户可能没开 VRCX，本地库不一定更新）：

| 需求 | 数据源 |
|---|---|
| 好友在线状态/位置 | **API** `GET /auth/user/friends`（实时） |
| 已访问过的世界（推荐去重） | **API** `GET /worlds/recent`（实时） |
| 世界/模型搜索、热度、收藏量 | **API** `GET /worlds` / `GET /avatars` |
| 用户当前头像/状态 | **API** `GET /users/{userId}` |
| 游戏日志（进出房间/视频播放历史） | VRCX 本地库 `gamelog_*` 表（VRCX 需要运行过才能记录） |
| 好友动态 Feed、活动热力图、备注、收藏 | VRCX 本地库 `<P>_feed_*` / `<P>_activity_*` 等表 |

原则：**能 API 拿到的实时数据不查本地库**；本地库只用于 API 没有的离线历史（日志、Feed、热力图）或 API 不稳定时的补充。

## 核心工作流

### 数据源选择：数据库 vs API（先想清楚再动手）

判断原则：**实时/当前状态用 API，历史/本地积累数据用数据库**。
VRCX 未运行时数据库不更新——实时性数据必须用 API；历史数据优先数据库。

**用 API（实时）**：
- 在线好友、用户当前状态/位置/当前头像
- 世界/模型的实时热度、当前在线人数
- 关键词搜索（搜世界/模型）、第三方库反查
- 通知（当前待处理的）

**用数据库（历史，VRCX 本地积累）**：
- **最近访问过的世界/实例**：`gamelog_location` 表——比 API `GET /worlds/recent` 更完整可靠（API 只反映近期会话，数据库记录了 VRCX 运行期全部历史；即使 VRCX 当前没开，历史数据依然准确）
- 玩家进出记录（`gamelog_join_leave`）、视频播放历史（`gamelog_video_play`）
- 好友动态 Feed（上下线/位置变化历史，`<P>_feed_*`）、活动热力图（`<P>_activity_*`）
- 头像使用历史（`<P>_avatar_history`）、备注、本地收藏、缓存的世界/模型详情（`cache_world`/`cache_avatar`，离线可用）

### A. 搜索 / 推荐世界（重点场景）

1. **确定搜索词**：用户说中文主题（如"恐怖"）时，扩展为英文关键词组合，VRChat 搜索按英文标签/名称匹配。恐怖类词表：`horror` `scary` `creepy` `haunted` `ghost` `paranormal` `dark` `nightmare`；可组合多个词分别搜索后合并去重。
2. **搜索**：`GET /worlds?search=<词>&n=100&sort=heat&order=descending&apiKey=...`（参数与字段见 `references/vrchat-api.md`）。一次拉 100 条再本地处理比多次小请求更省限流。
3. **时间过滤**：结果里 `publicationDate` 是 ISO8601 字符串，**未发布时为字符串 `"none"`**。过滤"最近 N 天"时先排除 `"none"`，再用 python 比较日期：
   ```bash
   python -c "from datetime import datetime,timezone,timedelta; cutoff=datetime.now(timezone.utc)-timedelta(days=30); ..."
   ```
4. **热度排序**：`heat`（近期热度 1-10）、`popularity`（综合受欢迎度 1-10）、`favorites`（收藏量）、`visits`（访问量，未登录为 0）。按场景选主排序键。
5. **排除已访问**：优先查 VRCX 数据库 `gamelog_location`（世界访问历史，本地积累最可靠——即使 VRCX 当前没开，历史也准确）；数据库无数据时用 API `GET /worlds/recent` 兜底。用 id 集合过滤掉推荐结果。
6. **呈现**：top 3-5 个，每行给出：名称、作者、收藏/访问量、发布时间、一句话描述、`worldId`（如 `wrld_xxx`）——worldId 用于用户确认后传送。

### B. 加入世界 / 传送

用户确认世界后，**优先用深链**（最简单可靠，VRChat 未运行也能唤起）：

```bash
start "" "vrchat://launch?id=wrld_xxx"   # 公开实例，无短名
```
或带实例短名（非公开实例需要，从 `GET instances/{worldId}:{instanceId}/shortName` 获取）：
```bash
start "" "vrchat://launch?id=wrld_xxx:12345~region(us)&shortName=abc12345"
```

进阶（VRChat 已在运行且需要精确控制实例）：`POST /instances` 创建实例 → `POST /invite/myself/to/{worldId}:{instanceId}` 自我传送。细节见 `references/vrchat-api.md`。

### C. 搜索模型 / 换模型

- 搜索：`GET /avatars?search=<词>&n=50&sort=heat&order=descending`
- 换模型（**先确认**）：`PUT /avatars/{avatarId}/select`（返回当前用户新信息）
- 看到某个模型想找来源/收藏/换装：走**第三方头像仓库反查**（工作流 F）

### D. 好友 / 通知 / 用户信息

- 好友列表：`GET /auth/user/friends?n=100`（**在线判定**：`location` 非空且不是 `"offline"`/`"private"`/`"traveling"` 才是真的在线——Web 端登录的朋友 `location` 可能为空，误判会把不在线的也算进去）
- 通知：`GET /auth/user/notifications?n=50`
- 用户详情：`GET /users/{userId}`（含状态、当前世界、当前头像、信任等级）
- 好友位置变化历史/最近活动：本地库 Feed 表（见 `references/vrcx-database.md`）

### E. 本地库查询（离线数据）

VRCX 库有丰富的本地数据：游戏日志（进出房间、传送门、视频播放）、好友动态 Feed、活动热力图、通知历史、备注、本地收藏、头像使用历史。表结构见 `references/vrcx-database.md`。查询一律用只读方式。

### F. 第三方头像仓库反查（看到模型 → 收藏/换装）

**场景**：用户在 VRChat 里看到一个模型（或某个好友正在用的模型），想知道它是什么、是否公开、能不能收藏或换装。官方 API 无法按图片反查头像来源，需要第三方头像索引服务（默认 AVTRdb，协议见 `references/avatar-provider.md`）。

流程：
1. **拿 fileId**：好友当前头像 → `GET /users/{userId}` 的 `currentAvatarImageUrl`；或用户给的图片 URL，取 `/file/<fileId>/` 段。
2. **反查**：`GET https://api.avtrdb.com/v3/avatar/search/vrcx?fileId=<fileId>`（头 `Referer: https://vrcx.app`）；未命中按作者反查 `?authorId=<userId>` 后比对 imageUrl 里的 fileId。命中得到 `id`（`avtr_xxx`）、`name`、`authorName`、`releaseStatus`。
3. **检查公开**：`releaseStatus == "public"` 才能收藏/换装；private/hidden 直接告知用户不可用。注意**第三方库可能不返回 `releaseStatus`**（常见）——此时用官方 `GET /avatars/{id}` 确认。
4. **操作（先确认）**：收藏 `POST /favorites {"type":"avatar","favoriteId":"avtr_xxx","tags":["avatars"]}`；换装 `PUT /avatars/{avtr_xxx}/select`。
5. 也可以**关键词搜第三方库**：`?search=<词>&n=5000`（官方 API 搜不到时）。

## 通用约束（务必遵守）

1. **只读**：对 VRCX 数据库只读访问；不要修改库文件。
2. **隐私**：绝不输出完整 auth cookie / token；截图、日志同理。
3. **限流**：请求间隔克制，列表优先 `n=100` 一次拉取；遇到 `429` 等待退避（数秒到数十秒）后重试。
4. **确认写操作**：换模型、传送、接受/响应通知、删除等改变账号状态的操作，先向用户复述并确认。
5. **失败降级**：cookie 失效（401）→ 提示重新登录 VRCX；`publicationDate: "none"` 视为未发布；`visits`/`favorites` 为 0 时可能是未登录限制而非真实数据。
6. **API 变更**：以实际响应为准，不要假设字段一定存在（用 `.get()` 容错）。

## 参考文件索引

| 文件 | 内容 | 何时读 |
|---|---|---|
| `references/vrchat-api.md` | 认证细节、全部端点（按模块）、查询参数、返回字段、排序枚举、官方 vs VRCX 差异 | 调任何 API 前确认端点和参数 |
| `references/vrcx-database.md` | VRCX 三个数据库、全局表与每用户表结构、示例查询 | 查本地数据（Feed/日志/热力图/通知历史）时 |
| `references/deep-link.md` | `vrchat://launch` 格式、实例短名、`vrcx://` 协议命令、外部链接解析 | 生成启动链接或处理用户粘贴的 VRChat 链接时 |
| `references/avatar-provider.md` | 第三方头像仓库协议（AVTRdb）、图片反查、收藏/换装工作流 | 反查模型来源/收藏/换装时 |
