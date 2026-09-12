# PLAN — 云端 ASR + 纯文本对话两段式（Remote ASR + Text-Only LLM）

> 状态：**已实施**（2026-09-12）。`llm.nativeAudio` 默认仍为 true；设为 false
> 时走“远程 ASR → 纯文本 LLM”，不依赖 sherpa 构建。

## 目标

把对话环节从多模态端点解耦：音频只用来**转写**（Remote ASR），得到的文字交给
任意便宜的纯文本 OpenAI 兼容端点（zen / OpenRouter 等）做对话。从而：
- 对话成本/可选厂商不再被支持 `input_audio` 的端点绑死；
- MiMo 只充当我们自己的"听写员"，按需使用。

## 实施前的差距

- 旧的 `llm.nativeAudio=false` 分支依赖本地 sherpa；本构建未启用 sherpa，
  该路径实际上不可用。
- 代码中没有独立的远程 ASR 配置和调用环节。

## 方案（实施步骤）

1. ✅ **agent-cpp 新增远程 ASR 前置调用**：把用户语音 wav 按 OpenAI `input_audio` 格式
   发给 ASR 端点（默认 MiMo），prompt 约"请把这段语音转成文字，只输出文字"，拿回
   文字作为对话输入。参考 `llm_client.hpp` 现有原生音频块构造逻辑复用。
2. ✅ **新增 `asr` 配置段**（`aoi_config.json.example` + `agent_config.hpp`）：
   ```
   "asr": { "baseUrl": "https://api.xiaomimimo.com/v1",
            "apiKey": "", "model": "mimo-v2.5" }
   ```
   `asr.apiKey` 缺省时回退用 `tts.apiKey`／`llm.apiKey`（同平台）。
3. ✅ **改 `nativeAudio=false` 分支**：不再依赖本地 sherpa，改走"ASR 转写 → 纯文本对话
   端点"。纯文本对话端点复用 `llm` 段（baseUrl 可指向 zen/OpenRouter 等）。
4. ✅ **保留** nativeAudio=true（直发多模态）与 false（两段式）双路线，配置可切。

## 实现结果

- `agent-cpp/src/remote_asr.*` 负责 data URL 解包、ASR 请求构造和 JSON/SSE
  转写结果解析。
- `nativeAudio=false` 在写入会话 history 前完成远程转写；LLM 请求只包含文本/图片，
  不再包含 `input_audio`。工具生成的音频也在下一轮发送前转换。
- native 音频端点返回包含 `input_audio` 的 HTTP 400 时，后续轮次 sticky fallback
  到远程 ASR，而不是依赖未编译的 sherpa。
- `asr.apiKey` 为空时按 `tts.apiKey` → `llm.apiKey` 回退；显式配置的 ASR key 优先。
- 新增 `aoi-remote-asr-tests`、`aoi-config-tests`，并纳入 `gate.cmd`。

## 不做什么

- 不改 sherpa 构建决策（VR 性能优先，仍不开）。
- 不动上游默认 nativeAudio=true 行为。

## 验证要点

- ✅ 协议单测：音频 data URL、`input_audio` 请求、JSON/SSE 响应解析。
- ✅ 配置单测：ASR 默认值、显式 key 和 TTS/LLM key fallback。
- ⏳ Windows 本机实测：中文语音准确度、真实 ASR/LLM 厂商解耦，以及 Player.log
  确认请求不含 `input_audio`；这仍需要交接文档规定的 Windows/MSVC + VR 环境。