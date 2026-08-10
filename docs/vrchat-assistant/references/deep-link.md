# 深链（Deep Link）机制参考

VRChat 相关的一切"启动"最终都归结为两类链接：**`vrchat://launch`（进游戏）** 和
**`vrcx://`（VRCX 应用内操作）**。本文件说明格式、参数与获取方法。

## `vrchat://launch` — 启动/加入世界

VRChat 客户端注册的协议，唤起后进入指定世界实例（VRChat 未运行时会先启动游戏）。

### 格式

```
vrchat://launch?ref=<来源>&id=<worldId>:<instanceId>&shortName=<短名>
```

| 参数 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | location 串：`wrld_xxx:instanceId`（如 `wrld_4432ea9b-729c-46e3-8eaf-846aa0a37fdd:12345~region(us)`；`~region(us)` 指定区域，可省略） |
| `shortName` | 非公开实例必须 | 实例短名（私有/好友房需要；公开实例可省略） |
| `ref` | 否 | 来源标记（VRCX 用 `ref=vrcx.app`，自定义用 `ref=aoi`） |

### 短名获取

```bash
curl -s "https://api.vrchat.cloud/api/1/instances/{worldId}:{instanceId}/shortName?apiKey=<key>" \
  -H "Cookie: auth=<token>" -H "User-Agent: ..."
```
- 返回 `{"shortName":"abc12345"}`；私有实例返回 `secureName`（更长的安全短名）。
- 也可反查：`GET instances/s/{shortName}`。

### 在 bash 中唤起

```bash
start "" "vrchat://launch?id=wrld_xxx:12345~region(us)"
# 或带短名：
start "" "vrchat://launch?id=wrld_xxx:12345&shortName=abc12345"
```

### 免短名的替代（不推荐）

`POST /invite/myself/to/{worldId}:{instanceId}` 自我传送——**要求 VRChat 客户端正在运行**，
且部分房间类型受限。深链是更通用的方案。

## `vrcx://` — VRCX 应用内命令

VRCX 注册的协议（系统点击 → 主进程剥离 `vrcx://` 前缀 → IPC 到 UI 执行）。
仅当 VRCX 已安装且登录时可用。

| 命令 | 参数 | 说明 |
|---|---|---|
| `vrcx://user/{userId}` | `usr_xxx` | 打开用户对话框 |
| `vrcx://world/{worldId}` | `wrld_xxx` 或完整 location | 打开世界对话框 |
| `vrcx://avatar/{avatarId}` | `avtr_xxx` | 打开模型对话框 |
| `vrcx://group/{groupId}` | `grp_xxx` | 打开群组对话框 |
| `vrcx://switchavatar/{avatarId}` | `avtr_xxx` | 切换模型（可配置确认框） |
| `vrcx://local-favorite-world/{worldId}:{group}` | — | 添加本地世界收藏 |
| `vrcx://local-favorite-avatar/{avatarId}:{group}` | — | 添加本地模型收藏 |
| `vrcx://import/{type}/{data}` | `avatar|world|friend` | 打开导入对话框 |

## 外部链接解析规则（VRCX 全局搜索框行为，可参考）

用户粘贴 VRChat 相关链接/ID 时按此解析目标：

| 输入 | 解析结果 |
|---|---|
| `https://vrchat.com/home/user/{userId}` | 用户 |
| `https://vrchat.com/home/avatar/{avatarId}` | 模型 |
| `https://vrchat.com/home/world/{worldId}` | 世界 |
| `https://vrchat.com/home/group/{groupId}` | 群组 |
| `https://vrchat.com/home/launch?worldId=&instanceId=&shortName=` | 实例 |
| `https://vrc.group/{shortCode}.{discriminator}` 或 `{CODE}.{disc}` | 群组短码反查（`GET groups/strictsearch`） |
| `usr_xxx` / 10 位字母数字 | 用户 |
| `avtr_xxx` / `b_xxx` | 模型 |
| `wrld_xxx` / `wld_xxx` / `o_xxx` | 世界 |
| `grp_xxx` | 群组 |
| 8 位字符 | 实例短名（`GET instances/s/{shortName}` 反查） |
| `https://vrch.at/{shortName}` | 实例短名 |

## 世界 ID 常识

- 世界 ID：`wrld_<uuid>`（旧世界可能是 `wld_` 或 `o_` 前缀）。
- 实例 ID：`<8位随机>~region(us|eu|jp)`（公开房）、`~hidden`（隐藏）、`~friends`（好友）、
  `~private`（私密）、`~group(grp_xxx)~region(us)`（群组房）。
- location 串 = `worldId:instanceId`（如 `wrld_xxx:12345~region(us)`）。
