# VRChat API 端点参考

> 依据官方 OpenAPI 规范（`https://vrchatapi.github.io/openapi.yaml`，2026-08，232 条路径）与
> VRCX 源码（`vrcx-team/VRCX`，Electron 分支）比对整理。所有路径相对
> `https://api.vrchat.cloud/api/1`。

## 认证与请求规范

- **API 密钥**：`GET /config` 返回（公开）。**字段名以实际响应为准**：旧版为 `apiKey`，当前已改名 `clientApiKey`。请求时带 `?apiKey=<值>` 或 `?clientApiKey=<值>` 兼容（多数端点现已不强制，但携带兼容性最好）。
- **auth cookie**：登录会话凭据。带 `Cookie: auth=<token>` 头。获取方式见 SKILL.md 认证章节。
- **User-Agent**：必须含联系方式（如 `AoiVR/0.1.0 (https://github.com/keybodhi/AoiVR)`），否则 401。
- **Content-Type**：POST/PUT 带 `Content-Type: application/json`，body 为 JSON。
- **限流**：有每分钟配额，超限返回 429，需退避重试。列表请求优先大 `n` 一次拉取。
- **未登录行为**：部分统计字段（`favorites`/`visits`）在未认证时返回 0；**当前 API 列表响应中 `visits` 已可能为 null**——以实际响应为准，`.get()` 容错。

## 端点一览（按模块）

### 认证 Auth

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `auth` | 检查认证状态（cookie 有效则返回 `{ok:true, token}`） |
| GET | `auth/user` | **登录/当前用户**：带 `Authorization: Basic base64(urlencode(user):urlencode(pass))` 即登录；已登录返回 CurrentUser |
| PUT | `logout` | 服务端注销（VRCX 只用本地清 cookie） |
| POST | `auth/twofactorauth/totp/verify` | TOTP 验证码验证 |
| POST | `auth/twofactorauth/otp/verify` | 恢复码验证 |
| POST | `auth/twofactorauth/emailotp/verify` | 邮件验证码验证 |
| GET | `config` | API 配置（apiKey、登录方式开关、2FA 类型） |

> **Session Limit 警告**：每次用账号密码认证计一个 session（有上限），
> 复用 cookie 不会新增 session。不要频繁重新登录。

### 用户 Users

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `users/{userId}` | 用户详情：状态（`status`）、当前世界（`location`/`worldId`）、当前头像、信任等级、平台、bio |
| GET | `users?search=&n=` | 按用户名搜索 |
| PUT | `users/{currentUserId}` | 更新当前用户（状态/Bio/头像等） |
| GET | `users/{username}/name` | 按用户名查用户 ID（无需 ID）★ |
| GET | `users/{userId}/groups` | 用户加入的群组 |
| GET | `users/{userId}/mutuals` (+`/friends` `/groups`) | 共同好友/群组 |
| GET/PUT/DELETE | `users/{currentUserId}/{worldId}/persist` | 世界持久化数据 |

### 模型 Avatars

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `avatars?search=&n=&sort=&order=` | **搜索模型**；`sort` 支持 `heat`/`popularity`/`favorites`/`created`/`updated`/`random` 等；`releaseStatus` 过滤 |
| GET | `avatars/{avatarId}` | 模型详情 |
| **PUT** | `avatars/{avatarId}/select` | **换模型**（选中即用，返回当前用户新信息） |
| PUT | `avatars/{avatarId}/selectFallback` | 换回回退模型 |
| PUT | `avatars/{avatarId}` | 更新模型信息 |
| GET | `avatars/favorites?featured=` | 收藏模型（可过滤官方精选） |
| GET | `avatars/impostor/queue/stats` | Impostor 队列统计 |

> Avatar 字段：`id(avtr_xxx)` `name` `description` `authorName` `thumbnailImageUrl` `imageUrl`
> `tags` `releaseStatus` `version` `created_at` `updated_at`。

### 世界 Worlds（核心）

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `worlds?search=&n=&sort=&order=&offset=&tag=&notag=&releaseStatus=&platform=&featured=&userId=` | **搜索世界**（详细参数见下） |
| GET | `worlds/{worldId}` | 世界详情 |
| GET | `worlds/recent` | **最近访问的世界列表**（去重推荐用） |
| GET | `worlds/active` | 当前活跃世界列表 |
| GET | `worlds/favorites?featured=` | 收藏世界 |
| PUT | `worlds/{worldId}` | 更新世界（作者） |

**搜索参数**：

| 参数 | 说明 |
|---|---|
| `search` | 关键词（名称/标签/描述模糊匹配） |
| `n` | 每页数量，默认 60，**上限 100** |
| `offset` | 分页偏移 |
| `sort` | 排序键：`heat` `popularity` `favorites` `created` `updated` `publicationDate` `random` `name` `relevance` 等 |
| `order` | `ascending` / `descending` |
| `featured` | 只看官方精选（boolean） |
| `tag` / `notag` | 按标签过滤/排除（可多个） |
| `releaseStatus` | `public` / `private` / `hidden` / `all` |
| `platform` | `standalonewindows` / `android` |
| `userId` | 按作者筛选（`user=me` 查自己的） |

**LimitedWorld 返回字段**（列表端点均为此模型）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string | `wrld_xxx` |
| `name` / `description` | string | 名称/描述 |
| `authorName` / `authorId` | string | 作者 |
| `capacity` / `recommendedCapacity` | int | 容量/推荐容量 |
| `occupants` | int | 当前在线人数 |
| `heat` | int | 近期热度（1-10） |
| `popularity` | int | 综合受欢迎度（1-10） |
| `favorites` | int | 收藏量（未登录为 0） |
| `visits` | int | 累计访问量（未登录为 0；**当前列表响应可能为 null**） |
| `publicationDate` | string | 发布时间 ISO8601；**未发布为 `"none"`** |
| `labsPublicationDate` | string | 进 Labs 时间（同 `"none"` 约定） |
| `created_at` / `updated_at` | string | 创建/更新时间 |
| `tags` | array | 标签（含 `author_tag_xxx` 等） |
| `thumbnailImageUrl` / `imageUrl` | string | 缩略图/大图 |
| `releaseStatus` | string | `public`/`private`/`hidden` |

### 实例 Instances

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `instances/{worldId}:{instanceId}` | 实例详情（人数、类型、群组信息） |
| **POST** | `instances` | **创建实例**：body `{"worldId":"wrld_xxx","type":"public|friends|private|group","region":"us|eu|jp"}`（group 需 `groupId`+`groupAccessType`） |
| GET | `instances/{worldId}:{instanceId}/shortName` | 实例短名（非公开实例给 `secureName`） |
| GET | `instances/s/{shortName}` | 短名反查实例 |
| GET | `instances/recent` | 最近实例 |

### 好友 Friends

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `auth/user/friends?n=&offset=` | 好友列表；**在线判定**：`location` 非空且非 `"offline"`/`"private"`/`"traveling"` 才是真的在线（Web 端登录者 location 可能为空）；字段含 `displayName` `userStatus` `platform` `location` `worldId` |
| DELETE | `auth/user/friends/{userId}` | 删除好友 |
| POST | `user/{userId}/friendRequest` | 发好友申请 |
| GET | `user/{userId}/friendStatus` | 好友关系状态 |

### 通知 Notifications

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `auth/user/notifications?n=` | 通知列表（v1：邀请/好友申请等） |
| POST | `auth/user/notifications/{id}/accept` | 接受（好友申请） |
| PUT | `auth/user/notifications/{id}/hide` | 隐藏 |
| PUT | `auth/user/notifications/{id}/see` | 标记已读 |
| POST | `invite/{receiverUserId}` | 发送邀请 |
| POST | `invite/myself/to/{worldId}:{instanceId}` | **自我邀请**（加入指定房间，兜底传送方案） |
| POST | `invite/{inviteId}/response` | 响应邀请 |

### 收藏 Favorites

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `favorites?n=&offset=` | 收藏列表（type: world/avatar/friend） |
| POST | `favorites` | 添加收藏 |
| DELETE | `favorites/{favoriteId}` | 删除收藏 |
| GET | `favorite/groups` | 收藏分组 |
| GET | `auth/user/favoritelimits` | 收藏限额 |

### 群组 Groups / 日历

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `groups?search=` | 搜索群组 |
| GET | `groups/{groupId}` | 群组详情 |
| GET | `groups/strictsearch?shortCode=` | **严格搜索（短码反查）**★ 官方文档未记载 |
| POST | `groups/{groupId}/join` / `leave` | 加入/退出 |
| GET | `groups/{groupId}/members` | 成员列表 |
| GET | `groups/{groupId}/instances` | 群组实例 |
| GET/POST | `calendar/{groupId}` (+`/event`) | 群组日历/活动 |

### 杂项 Misc

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `auth/user/playermoderations` | 我的屏蔽/静音列表 |
| POST | `auth/user/playermoderations` | block/mute/unblock/unmute |
| GET | `user/{currentUserId}/economy/balance` | 钱包余额 |
| GET | `visits` | 访问统计 |
| GET | `health` / `time` | API 健康/服务器时间 |
| GET | `infoPush` | 官方公告 |
| GET/PUT | `message/{userId}/{type}` | 邀请消息模板 |

## 官方文档未记载的端点（VRCX 实战使用）

这些在官方 OpenAPI 查不到，但 VRChat 客户端/生态真实使用：

| 方法 | 端点 | 说明 |
|---|---|---|
| GET | `groups/strictsearch` | 群组短码严格反查（`vrc.group/CODE.disc` 跳转依赖） |
| GET | `inventory/global` | 全局物品目录 |
| GET/PUT | `profile/theme` (+`/{themeId}`) | 个人主页主题 |
| GET | `user/{userId}/economy/balance` | 钱包余额变体 |
| POST | `feedback/{userId}/user` | 提交用户反馈 |

## 常见坑

1. `GET /worlds?search=` 的 `sort=heat` 等值大小写敏感，`order` 传 `descending`。
2. `n` 上限 100，超过会被截断或报错。
3. `publicationDate: "none"` 表示从未发布（Labs 状态），按发布时间过滤必须先排除。
4. `PUT /avatars/{avatarId}/select` 是 PUT 不是 POST；`selectFallback` 大小写敏感。
5. 未认证时 `visits`/`favorites` 返回 0 或 null，不代表真实数据。
6. 429 退避：等待后重试；持续 429 说明请求太频繁。
7. `invite/myself/to/{location}` 需要 VRChat 客户端在运行；深链（`vrchat://launch`）不需要。
