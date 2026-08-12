# VRCX 数据库参考

> 基于 `vrcx-team/VRCX` 源码（Electron 分支，2026-08）整理。VRCX 有 **3 个 SQLite 数据库**，
> 与 AI 相关的是主库。所有表结构来自 `src/services/database/index.js`（建表）、
> `Dotnet/WebApi.cs`（cookies 表）与 `Dotnet/ScreenshotMetadata/`。

## 数据库文件

| 数据库 | 位置 | 用途 |
|---|---|---|
| `VRCX.sqlite3` | `%APPDATA%\VRCX\`（可用 `VRCX.json` 的 `VRCX_DatabaseLocation` 自定义，注意 JSON 中反斜杠双写） | 主库：全部业务数据 |
| 截图元数据库 | 截图功能目录下 | 截图→元数据缓存 |
| 登录凭据 | `configs` 表 + `cookies` 表 | 账号凭据持久化 |

> 注意：VRCX 运行中时主库处于 WAL 模式（伴随 `-wal`/`-shm` 文件），**只读打开是安全的**；
> 不要以写模式打开，不要删除 WAL 文件。

## 全局表（所有账号共享）

### `configs` — KV 配置

```sql
CREATE TABLE IF NOT EXISTS configs (`key` TEXT PRIMARY KEY, `value` TEXT);
```
典型 key：`lastUserLoggedIn`、`VRCX_databaseVersion`（当前 16）、`VRCX_maxTableSize_v2`、
`VRCX_searchLimit`、`launchArguments`、`vrcLaunchPathOverride` 等。

### `cookies` — 登录会话（**认证关键表**）

```sql
CREATE TABLE IF NOT EXISTS `cookies` (`key` TEXT PRIMARY KEY, `value` TEXT);
```
- 单行：`key = 'default'`。
- `value` = **Base64( JSON 数组 )**，数组元素是 .NET `System.Net.Cookie` 序列化：
  ```json
  [{"Name":"auth","Value":"<token>","Domain":"api.vrchat.cloud","Path":"/",
    "Expires":"9999-12-31T23:59:59.9999999","HttpOnly":true,"Secure":false,
    "TimeStamp":"2026-08-09T23:02:34.6332976Z","Version":0}, ...]
  ```
- 字段为**大写开头**（System.Text.Json 序列化 .NET 属性名）：`Name` `Value` `Domain` `Path`
  `Expires` `HttpOnly` `Secure` `TimeStamp` `Comment` `Discard` `Port` `Version`。
- VRChat API 凭据 = `Name=="auth"` 且 `Domain` 含 `vrchat.cloud` 的 `Value`。
- 提取命令模板（python 一步）见 SKILL.md 认证章节。

### 游戏日志表（LogWatcher 解析 `output_log.txt` 写入）

```sql
-- 位置变化（含停留秒数）
CREATE TABLE gamelog_location (id INTEGER PRIMARY KEY, created_at TEXT, location TEXT,
    world_id TEXT, world_name TEXT, time INTEGER, group_name TEXT,
    UNIQUE(created_at, location));
-- 玩家进出
CREATE TABLE gamelog_join_leave (id INTEGER PRIMARY KEY, created_at TEXT, type TEXT,
    display_name TEXT, location TEXT, user_id TEXT, time INTEGER,
    UNIQUE(created_at, type, display_name));
-- 传送门生成
CREATE TABLE gamelog_portal_spawn (id INTEGER PRIMARY KEY, created_at TEXT, display_name TEXT,
    location TEXT, user_id TEXT, instance_id TEXT, world_name TEXT,
    UNIQUE(created_at, display_name));
-- 视频播放（video_url/video_name/video_id）
CREATE TABLE gamelog_video_play (id INTEGER PRIMARY KEY, created_at TEXT, video_url TEXT,
    video_name TEXT, video_id TEXT, location TEXT, display_name TEXT, user_id TEXT,
    UNIQUE(created_at, video_url));
-- 资源加载（图片/字符串 URL）
CREATE TABLE gamelog_resource_load (id INTEGER PRIMARY KEY, created_at TEXT, resource_url TEXT,
    resource_type TEXT, location TEXT, UNIQUE(created_at, resource_url));
-- 自定义事件 / 外部消息（Mod IPC）
CREATE TABLE gamelog_event (id INTEGER PRIMARY KEY, created_at TEXT, data TEXT, UNIQUE(created_at, data));
CREATE TABLE gamelog_external (id INTEGER PRIMARY KEY, created_at TEXT, message TEXT,
    display_name TEXT, user_id TEXT, location TEXT, UNIQUE(created_at, message));
```

> 用途示例：回答"我刚才在哪个世界"（gamelog_location 按 created_at 倒序）、
> "xx 什么时候进过我的房间"（gamelog_join_leave）、"刚才放的什么视频"（gamelog_video_play）。

### 内容缓存与本地收藏

```sql
-- 世界/模型缓存（结构相同）：无网络也能查收藏/浏览记录
CREATE TABLE cache_world (id TEXT PRIMARY KEY, added_at TEXT, author_id TEXT, author_name TEXT,
    created_at TEXT, description TEXT, image_url TEXT, name TEXT, release_status TEXT,
    thumbnail_image_url TEXT, updated_at TEXT, version INTEGER);
CREATE TABLE cache_avatar (id TEXT PRIMARY KEY, ...同结构...);

-- 本地收藏（区别于 API 收藏，深链 vrcx://local-favorite-* 可添加）
CREATE TABLE favorite_world (id INTEGER PRIMARY KEY, created_at TEXT, world_id TEXT, group_name TEXT);
CREATE TABLE favorite_avatar (id INTEGER PRIMARY KEY, created_at TEXT, avatar_id TEXT, group_name TEXT);
CREATE TABLE favorite_friend (id INTEGER PRIMARY KEY, created_at TEXT, user_id TEXT, group_name TEXT);

-- 备忘录（用户/世界/模型）
CREATE TABLE memos (user_id TEXT PRIMARY KEY, edited_at TEXT, memo TEXT);
CREATE TABLE world_memos (world_id TEXT PRIMARY KEY, edited_at TEXT, memo TEXT);
CREATE TABLE avatar_memos (avatar_id TEXT PRIMARY KEY, edited_at TEXT, memo TEXT);

-- 模型自定义标签
CREATE TABLE avatar_tags (avatar_id TEXT NOT NULL, tag TEXT NOT NULL, color TEXT,
    PRIMARY KEY (avatar_id, tag));
```

## 每用户表（登录后每个账号各建一套）

**表名前缀** = 用户 ID 去掉 `-` 和 `_`（如 `usr_abc123-456` → `usrabc123456_`），数字开头加 `_`。
以下 `<P>` 即前缀。先 `SELECT name FROM sqlite_master WHERE name LIKE '%feed%'` 之类的查询可确认实际表名。

### Feed 系列（好友动态，WebSocket + 周期轮询写入）

```sql
CREATE TABLE <P>_feed_gps (id INTEGER PRIMARY KEY, created_at TEXT, user_id TEXT,
    display_name TEXT, location TEXT, world_name TEXT, previous_location TEXT,
    time INTEGER, group_name TEXT);              -- 位置变化
CREATE TABLE <P>_feed_status (id INTEGER PRIMARY KEY, created_at TEXT, user_id TEXT,
    display_name TEXT, status TEXT, status_description TEXT,
    previous_status TEXT, previous_status_description TEXT);  -- 状态变化
CREATE TABLE <P>_feed_bio (id INTEGER PRIMARY KEY, created_at TEXT, user_id TEXT,
    display_name TEXT, bio TEXT, previous_bio TEXT);          -- 简介变化
CREATE TABLE <P>_feed_avatar (id INTEGER PRIMARY KEY, created_at TEXT, user_id TEXT,
    display_name TEXT, owner_id TEXT, avatar_name TEXT,
    current_avatar_image_url TEXT, current_avatar_thumbnail_image_url TEXT,
    previous_current_avatar_image_url TEXT, previous_current_avatar_thumbnail_image_url TEXT);  -- 换模型
CREATE TABLE <P>_feed_online_offline (id INTEGER PRIMARY KEY, created_at TEXT, user_id TEXT,
    display_name TEXT, type TEXT, location TEXT, world_name TEXT, time INTEGER,
    group_name TEXT);                            -- 上下线（type: online/offline）
```

### Activity v2（活动热力图）

```sql
CREATE TABLE <P>_activity_sync_state_v2 (user_id TEXT PRIMARY KEY, updated_at TEXT DEFAULT '',
    is_self INTEGER DEFAULT 0, source_last_created_at TEXT DEFAULT '',
    pending_session_start_at INTEGER, cached_range_days INTEGER DEFAULT 0);
CREATE TABLE <P>_activity_sessions_v2 (session_id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id TEXT NOT NULL, start_at INTEGER NOT NULL, end_at INTEGER NOT NULL,
    is_open_tail INTEGER DEFAULT 0, source_revision TEXT DEFAULT '');   -- 在线会话（时间戳）
CREATE TABLE <P>_activity_bucket_cache_v2 (user_id TEXT NOT NULL, target_user_id TEXT DEFAULT '',
    range_days INTEGER, view_kind TEXT, exclude_key TEXT DEFAULT '', bucket_version INTEGER DEFAULT 1,
    raw_buckets_json TEXT DEFAULT '[]', normalized_buckets_json TEXT DEFAULT '[]',
    built_from_cursor TEXT DEFAULT '', summary_json TEXT DEFAULT '{}', built_at TEXT DEFAULT '',
    PRIMARY KEY (user_id, target_user_id, range_days, view_kind, exclude_key));
```
> 用途：回答"我最近几点上线多"、"朋友的在线规律"（`activity_sessions_v2` 按 start_at/end_at 聚合）。

### Friend Log（好友历史）

```sql
CREATE TABLE <P>_friend_log_current (user_id TEXT PRIMARY KEY, display_name TEXT,
    trust_level TEXT, friend_number INTEGER);
CREATE TABLE <P>_friend_log_history (id INTEGER PRIMARY KEY, created_at TEXT, type TEXT,
    user_id TEXT, display_name TEXT, previous_display_name TEXT, trust_level TEXT,
    previous_trust_level TEXT, friend_number INTEGER);   -- 加友/删友/改名/信任变化
```

### 通知（本地历史）

```sql
-- v1（旧格式）
CREATE TABLE <P>_notifications (id TEXT PRIMARY KEY, created_at TEXT, type TEXT,
    sender_user_id TEXT, sender_username TEXT, receiver_user_id TEXT, message TEXT,
    world_id TEXT, world_name TEXT, image_url TEXT, invite_message TEXT,
    request_message TEXT, response_message TEXT, expired INTEGER);
-- v2（Notification 2.0）
CREATE TABLE <P>_notifications_v2 (id TEXT PRIMARY KEY, created_at TEXT, updated_at TEXT,
    expires_at TEXT, type TEXT, link TEXT, link_text TEXT, message TEXT, title TEXT,
    image_url TEXT, seen INTEGER, sender_user_id TEXT, sender_username TEXT,
    data TEXT, responses TEXT, details TEXT);
```

### 其他

```sql
CREATE TABLE <P>_moderation (user_id TEXT PRIMARY KEY, updated_at TEXT, display_name TEXT,
    block INTEGER, mute INTEGER);                              -- 屏蔽/静音
CREATE TABLE <P>_avatar_history (avatar_id TEXT PRIMARY KEY, created_at TEXT, time INTEGER);  -- 用过哪些模型
CREATE TABLE <P>_notes (user_id TEXT PRIMARY KEY, display_name TEXT, note TEXT, created_at TEXT);  -- 好友备注
CREATE TABLE <P>_mutual_graph_friends (friend_id TEXT PRIMARY KEY);      -- 共同好友图谱
CREATE TABLE <P>_mutual_graph_links (friend_id TEXT NOT NULL, mutual_id TEXT NOT NULL, PRIMARY KEY(friend_id, mutual_id));
CREATE TABLE <P>_mutual_graph_meta (friend_id TEXT PRIMARY KEY, last_fetched_at TEXT, opted_out INTEGER DEFAULT 0);
```

## 查询技巧

1. **找表**：`SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'`
2. **确认前缀**：`SELECT name FROM sqlite_master WHERE type='table' AND name LIKE '%_feed_gps%'`
3. **只读打开**（python）：`sqlite3.connect(f'file:{path}?mode=ro', uri=True)`
4. **最近去过的世界**：`SELECT world_name, location, created_at, time FROM gamelog_location ORDER BY created_at DESC LIMIT 20`
5. **最近进出的人**：`SELECT display_name, type, location, created_at FROM gamelog_join_leave ORDER BY created_at DESC LIMIT 20`
6. **我的模型使用史**：`SELECT * FROM <P>_avatar_history ORDER BY created_at DESC`
7. 时间列 `created_at` 为 ISO8601 字符串（如 `2026-08-10T01:02:03.123Z`）；`time` 为停留秒数；activity 表用 unix 时间戳（INTEGER）。

## 安全

- 永远只读。VRCX 运行中写库可能损坏数据或引发冲突。
- `configs` 表可能含 `loginParams`（加密的账号密码残留）——**不要读取或输出该 key 的内容**。
