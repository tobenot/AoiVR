# Aoi VR Agent

> **一个住在你 SteamVR 手里的 AI 语音助手。**

全息手板面板附着在你的控制器上——由本地原生 C++ agent 驱动（内置 LLM 客户端、
流式 TTS、VR 截图、手势控制）。跟它对话、让它看看你的 VR 空间、或者控制你的
系统——一切尽在掌中。

![Aoi VR Agent 演示](assets/demo.gif)

## 功能特性

| | |
|---|---|
| 🎙️ **语音对话** | LLM 对话（OpenAI 兼容，针对 opencode zen/go 网关优化：prompt 缓存、流式推理、用量统计）+ 流式 TTS |
| 🖐️ **SteamVR 手板面板** | 附着在右手控制器的 OpenVR overlay——激光/射线输入、桌面镜像 |
| 👁️ **环境感知** | 按需 VR 截图 + 可选持续视觉 & 系统音频转写 |
| 🌐 **同声传译** | 实时翻译系统音频 |
| 🔊 **系统控制** | 应用级音量（Windows 音量合成器）、VR 亮度调节 |
| 🧩 **原生核心** | `aoi_agent.dll`（C++20）——自包含、进程内 agent 循环（绝不阻塞 Unity 渲染线程） |

![手板截图](assets/screenshot-2.png)

## 快速开始（预编译版）

从 [Releases 页面](https://github.com/keybodhi/aoiVR/releases) 下载最新发布包，
解压后运行：

```
launch.bat
```

首次运行会从模板创建 `aoi_config.json`——填入你的 LLM / TTS API key（见
`aoi_config.json.example`），再次运行即可。也可以直接启动：

```
AoiVR.exe                # SteamVR overlay 模式
AoiVR.exe -desktop       # 桌面预览窗口（无需头显）
AoiVR.exe -desktop -demo # 演示模式（无需 API key）
```

`-demo` 模式在面板上播放一段模拟对话——无需任何 key 即可体验界面，也方便录制演示。

### 语音输入路线

默认 `llm.nativeAudio` 为 `true`：原始语音直接发给支持 `input_audio` 的多模态
端点。若要把 ASR 和对话模型解耦，把它设为 `false`，并配置：

```json
{
  "llm": {
    "baseUrl": "https://opencode.ai/zen/go/v1",
    "apiKey": "<text-llm-key>",
    "nativeAudio": false
  },
  "asr": {
    "baseUrl": "https://api.xiaomimimo.com/v1",
    "apiKey": "<asr-key>",
    "model": "mimo-v2.5"
  }
}
```

此模式会先用 ASR 端点转写，再把纯文字交给 `llm` 端点；`asr.apiKey` 留空时会
依次回退到 `tts.apiKey`、`llm.apiKey`。LLM 请求不会包含 `input_audio`，因此可以
使用不支持音频输入的纯文本网关。

## 从源码构建

环境要求：Visual Studio（MSVC + CMake）、Unity 6000.5（IL2CPP Windows
Standalone）、SteamVR。

### 1. 原生 agent DLL

```bat
cd agent-cpp
cmake -S . -B build
cmake --build build --config Release --target aoi-agent-dll
```

或者用 `agent-cpp\gate.cmd` 运行完整质量门禁（配置 + 构建 + 全部测试套件）。

### 2. Unity 客户端

1. 复制 `agent-cpp/build/Release/aoi_agent.dll` 到
   `unity-client/Assets/Aoi/Plugins/x86_64/aoi_agent.dll`
2. 用 Unity 6000.5 打开 `unity-client`，运行 `Build/Build Aoi`（菜单），或命令行：

```bat
Unity.exe -batchmode -nographics -quit ^
  -projectPath unity-client -executeMethod BuildScript.Build
```

输出：`unity-client/Build/AoiVR.exe`。

构建会自动把以下内容复制到输出目录（随发布包分发）：
- `aoi_config.json.example`（运行时配置模板）
- `docs/vrchat-assistant/`（VRChat 集成技能文档——agent 运行时通过
  `read` 工具按需加载，用于 VRChat 世界/模型/好友等操作；源码同步维护在
  `docs/vrchat-assistant/`，勿在输出目录手改）

## 仓库结构

| 路径 | 说明 |
|---|---|
| `agent-cpp/` | 原生 C++ agent（LLM 客户端、TTS、ASR、DSP、工具），导出为 `aoi_agent.dll`；系统提示词在 `src/prompts.hpp` |
| `unity-client/` | Unity 工程（SteamVR overlay UI、手板面板、agent 桥接） |
| `docs/vrchat-assistant/` | VRChat 集成技能文档（agent 运行时 `read` 加载；构建时自动复制进产物） |
| `assets/` | 演示录制与截图 |

## 许可证

MIT — 见 [LICENSE](LICENSE)。第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，
许可原文在 `licenses/`。
