# 第三方头像仓库（Avatar Provider）参考

> 依据 VRCX 源码（`src/stores/avatarProvider.js`、`src/coordinators/avatarCoordinator.js`）整理。
> VRChat 官方 API 无法"按图片反查"头像来源，也无法跨账号查别人正在用的模型；
> 第三方头像索引服务（默认 **AVTRdb**）弥补了这一点。

## 机制

VRCX 定义了一套**极简 HTTP 协议**，任何第三方提供者实现该协议即可被使用。
VRCX 本身不索引，只负责查询与展示。

- 默认提供者：`https://api.avtrdb.com/v3/avatar/search/vrcx`
- 配置存储：VRCX 库 `configs` 表，key `VRCX_avatarRemoteDatabaseProviderList`（JSON 字符串数组）
- 添加 provider 深链：`vrcx://addavatardb/{url}`（URL 编码）
- 用本技能时无需改 VRCX 配置——直接向提供者 URL 发 HTTP 请求即可。

## 协议

### 请求

```
GET {providerUrl}?search={关键词}&n=5000    -- 关键词搜索
GET {providerUrl}?authorId={userId}         -- 按作者反查
GET {providerUrl}?fileId={fileId}           -- 按图片文件 ID 反查（精确）
```

统一请求头：

| 头 | 值 |
|---|---|
| `Referer` | `https://vrcx.app` |
| `VRCX-ID` | VRCX 安装 ID（可选，用于统计；从 VRCX 数据中取不到就省略） |

### 响应

- `search` / `authorId` → **JSON 数组**（每个元素一个头像对象）
- `fileId` → **单个 JSON 对象**（或数组）
- 200 且 JSON 才有效；`fileId` 查询失败可能返回 null（部分提供者不支持该查询）

### 头像对象字段（注意 Id 大小写）

```json
{
  "Id": "avtr_xxx",            // search 返回用大写 Id（去重键）；authorId/fileId 用小写 id
  "id": "avtr_xxx",
  "authorId": "usr_xxx",
  "authorName": "作者名",
  "name": "头像名",
  "description": "描述",
  "imageUrl": "https://api.vrchat.cloud/api/1/file/file_xxx/1/image",
  "thumbnailImageUrl": "https://api.vrchat.cloud/api/1/file/file_xxx/1/thumbnail",
  "releaseStatus": "public",   // public / private / hidden
  "created_at": "...", "updated_at": "..."
}
```

## 工作流：图片/模型反查 → 收藏或换装

**典型场景**：用户在 VRChat 看到（或好友正在用）某个模型，想知道来源、是否公开、能否收藏/换装。

### 第 1 步：拿到 fileId（图片文件 ID）

来源：

- **好友当前头像**：`GET /users/{userId}` 响应的 `currentAvatarImageUrl` / `currentAvatarThumbnailImageUrl`
- **用户给的图片 URL**：任意 `https://api.vrchat.cloud/api/1/file/{fileId}/...` 形式的链接
- 提取：URL 中 `/file/<fileId>/` 段（如 `file_file_9e2f.../1/image` → fileId = `file_9e2f...`）

### 第 2 步：第三方库反查（fileId → 头像）

```
fetch(url="https://api.avtrdb.com/v3/avatar/search/vrcx?fileId=<fileId>",
      headers=["Referer: https://vrcx.app",
               "User-Agent: AoiVR/0.1.0 (https://github.com/keybodhi/AoiVR)"])
```

未命中时回退：按作者反查后再比对 imageUrl 中的 fileId：

```
fetch(url="https://api.avtrdb.com/v3/avatar/search/vrcx?authorId=<ownerUserId>",
      headers=["Referer: https://vrcx.app"])
# 然后在返回的 json 里找 imageUrl 含该 fileId 的条目（用 convert json_query 或 python）
```

命中 → 拿到 `id`（`avtr_xxx`）、`name`、`authorName`、`releaseStatus`。

### 第 3 步：检查是否公开

`releaseStatus == "public"` 才可收藏/换装；`private`/`hidden` 告诉用户该模型不可公开使用。

### 第 4 步：操作（先向用户确认）

**收藏**（POST 官方 API）：

```
fetch(url="https://api.vrchat.cloud/api/1/favorites?apiKey=<key>",
      method="POST",
      headers=["Cookie: auth=<token>",
               "User-Agent: AoiVR/0.1.0 (https://github.com/keybodhi/AoiVR)",
               "Content-Type: application/json"],
      body="{\"type\":\"avatar\",\"favoriteId\":\"avtr_xxx\",\"tags\":[\"avatars\"]}")
```

**换装**（PUT 方法，fetch 只支持 GET/POST——用 python 发起；python 自带 OpenSSL，沙箱内可用）：

```bash
python -c "import urllib.request; r=urllib.request.Request('https://api.vrchat.cloud/api/1/avatars/avtr_xxx/select?apiKey=<key>', method='PUT', headers={'Cookie':'auth=<token>','User-Agent':'AoiVR/0.1.0 (https://github.com/keybodhi/AoiVR)'}); print(urllib.request.urlopen(r, timeout=20).read().decode())"
```

（认证/apiKey 细节见 SKILL.md 认证章节与 vrchat-api.md。）

### 关键词搜索第三方库（官方 API 之外）

```
fetch(url="https://api.avtrdb.com/v3/avatar/search/vrcx?search=<关键词>&n=5000",
      headers=["Referer: https://vrcx.app"],
      max_bytes=500000)
```
适合官方 API 搜不到、或想找特定来源/特定作者的模型时使用。n 上限 5000。

## 注意事项

- 图片 URL 必须是 VRChat File API 格式（`/file/{fileId}/...`），否则图片反查失效。
- 第三方库数据是**索引快照**，可能与官方状态有出入——以 `GET /avatars/{id}` 的 `releaseStatus` 为准。
- 不要对提供者高频请求；一次 `n=5000` 关键词搜索足以覆盖。
- 提供者服务不稳定时（超时/非 JSON），提示用户稍后重试，不要反复重试。
