# 功能:英文谐音标注 (agent-cpp)

用户请求翻译成英文(或问英文怎么读)时,输出 [PRONUNCIATION] 块:
每行四要素 = 英文原句 | 谐音 | IPA | 中文意思(双空格分隔)。
面板显示四要素(去掉标记行),TTS 只朗读每行第一列(英文原句,跟读示范)。

## 场景:块输出格式

- 翻译成英文/问读音时输出 [PRONUNCIATION] ... [PRONUNCIATION_END] 块,每行一句
- 每行四列:英文原句  谐音  IPA  中文意思(双空格分隔)
- 谐音用英文音节拆分式(low-bay 式),如 rendezvous -> ron-day-voo,不用中文同音字
- IPA 用宽式音标,如 /ˈrɒn.deɪ.vuː/
- 普通中文对话/同传翻译不输出块,保持 VOICE 风格(纯文本、无 markdown、无 emoji)

## 场景:显示与朗读分离

- 面板显示:去掉 [PRONUNCIATION] 和 [PRONUNCIATION_END] 标记行,显示四要素行
- TTS 朗读:块内每行只朗读第一列(英文原句),谐音/IPA/中文意思不朗读
- 块内按行处理,英文句中的句号不触发切句
- 无标记的普通回复:流式逐句朗读,行为与上游一致

## 场景:知识库注入(有文件)

- 配置 knowledgeBase 指向存在的 UTF-8 文件时,system prompt 注入 "Meeting knowledge base (private terms the user provided):" 段
- 文件每行四列:中文 | English | 日本語 | 中文解释(竖线分隔;英文/日文列可空,解释列必填)
- 注入为 "- 词: 解释 (English: ..., 日本語: ...)" 列表,词头取中文列(空则取英文/日文)
- "#" 开头的行为注释,跳过

## 场景:知识库注入(无文件)

- knowledgeBase 为空,或文件不存在/为空时,不注入任何内容,system prompt 与上游完全一致
