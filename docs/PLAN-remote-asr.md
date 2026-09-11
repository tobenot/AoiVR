# PLAN — 云端 ASR + 纯文本对话两段式（Remote ASR + Text-Only LLM）

> 状态：**方案，待实施**。触发条件：MiMo 多模态 API 额度紧张时开工。
> 现状：llm.nativeAudio 默认 true，本地 sherpa 转写未编译（AOI_ENABLE_SHERPA=OFF），
> 语音直发 MiMo 多模态端点（已实测打通）。

## 目标

把对话环节从多模态端点解耦：音频只用来**转写**（Remote ASR），得到的文字交给
任意便宜的纯文本 OpenAI 兼容端点（zen / OpenRouter 等）做对话。从而：
- 对话成本/可选厂商不再被支持 `input_audio` 的端点绑死；
- MiMo 只充当我们自己的"听写员"，按需使用。

## 现状与差距

- 现 `llm.nativeAudio=false` 分支走的是**本地 sherpa 转写**；本构建未启用 sherpa，
  该路径是死的（返回 unavailable）。
- 代码里**不存在"远程 ASR"环节**。`llm_client.hpp` / `agent_config.hpp` 目前只有
  多模态直发（nativeAudio=true）和本地转写（false）两个分支。

## 方案（实施步骤）

1. **agent-cpp 新增远程 ASR 前置调用**：把用户语音 wav 按 OpenAI `input_audio` 格式
   发给 ASR 端点（默认 MiMo），prompt 约"请把这段语音转成文字，只输出文字"，拿回
   文字作为对话输入。参考 `llm_client.hpp` 现有原生音频块构造逻辑复用。
2. **新增 `asr` 配置段**（`aoi_config.json.example` + `agent_config.hpp`）：
   ```
   "asr": { "baseUrl": "https://api.xiaomimimo.com/v1",
            "apiKey": "", "model": "mimo-v2.5" }
   ```
   `asr.apiKey` 缺省时回退用 `tts.apiKey`／`llm.apiKey`（同平台）。
3. **改 `nativeAudio=false` 分支**：不再依赖本地 sherpa，改走"ASR 转写 → 纯文本对话
   端点"。纯文本对话端点复用 `llm` 段（baseUrl 可指向 zen/OpenRouter 等）。
4. **保留** nativeAudio=true（直发多模态）与 false（两段式）双路线，配置可切。

## 不做什么

- 不改 sherpa 构建决策（VR 性能优先，仍不开）。
- 不动上游默认 nativeAudio=true 行为。

## 验证要点

- 转写准确：中文语音 → 文字无遗漏/错别字。
- 对话走纯文本端点：Player.log 应显示请求不含 input_audio、音频计费不复现。
- `asr` / `llm` 可分别指向不同厂商验证解耦。