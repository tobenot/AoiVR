# 功能:英文谐音标注 (agent-cpp)

输出英文文本(如翻译成英文、用户要照念的英文)时,**自动**按
"英文原句  谐音  IPA" 双显格式附带读音标注,无需用户每句要求。
普通中文/非英文回复保持普通 VOICE 对话,不附带。

## 场景:自动触发

- 输出英文文本(翻译成英文、英文回复)时自动附带双显,不用用户要求
- 用户问 "这句话怎么读" / "标谐音" / "怎么发音" / "how do I say this" 时同样输出
- 普通中文回复不附带,保持 VOICE 风格(纯文本、无 markdown、无 emoji)

## 场景:双显输出格式

- 每行一个单词或句子:英文原句  谐音  IPA,以 2 空格或 tab 分隔
- 谐音用英文音节拆分式读音(low-bay 式),如 rendezvous -> ron-day-voo、conference -> kon-fer-ence、entrepreneur -> on-truh-pruh-nur
- 谐音不用中文同音字(用户要照念英文)
- IPA 用宽式音标,如 /ˈrɒn.deɪ.vuː/、/ˈkɒn.fər.əns/

## 场景:知识库注入(有文件)

- 配置 knowledgeBase 指向存在的 UTF-8 文件时,system prompt 注入 "Meeting knowledge base (private terms the user provided):" 段
- 文件每行 "词 | 简短解释"(竖线分隔),注入为 "- 词: 解释" 列表
- 无 "|" 的行、空词或空解释的行被跳过,不影响其余条目

## 场景:知识库注入(无文件)

- knowledgeBase 为空,或文件不存在/为空时,不注入任何内容,system prompt 与上游完全一致
