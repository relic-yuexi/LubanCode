# 回合总结与记忆候选提取

你拿到的是一段终端 AI 编程工具里刚结束的一个回合(用户消息、助手回答、工具调用摘要)。请先在心里推测用户这一回合的目的,再按下面分型提示词的侧重,产出一回合总结,并提取值得长期记住的候选。

## 输出格式(严格遵守)

只输出一个 JSON object,不加代码围栏、不加任何解释文字:

```
{"task_type":"code|research|config|docs|other","summary":"不超过 120 字的回合总结","retrieval_terms":["下一轮检索用的关键词或同义改写,最多 8 个"],"candidates":[{"kind":"fact|preference|feedback","title":"短主题","summary":"一行摘要","content":"精炼正文,含 ## Why 小节写来龙去脉(没有根据就省略,不编故事)","keywords":["精确检索词"],"paths":["支撑证据的项目内相对路径"],"occurred_at":"事件发生日期,材料里明确给出才填(YYYY-MM-DD 或 ISO 时间)","confidence":"user-stated|verified|inferred"}]}
```

candidates 可以为空数组,最多 3 条。retrieval_terms 是给下一轮记忆检索用的扩展词:同义词、更标准的叫法、涉及的符号名/路径,不要放整句话。occurred_at 只认材料里明确写出的日期(如"5月8日发布""2023-07-01 上线");材料没写、只有"上周""前几天"这类相对说法时省略该字段,不许推算或猜测日期。

## 摘要与标题的成色(检索命中靠它们)

summary 与 candidates 的 summary/title 是记忆检索的词面,写得虚,下次就查不到。按要点写:

- 谁:实体用原词——人名、项目名、模块名、符号名照抄,不写成"某人""某模块"。
- 何时:材料里有日期就把日期原词写进 summary(与 occurred_at 同款规矩,没有就省略)。
- 何地/何事:一句话说清在哪、做了什么、结果如何。
- candidates 的 title 必含核心实体(人名/模块/符号至少一个),不写"一次经验""某个事实"这类空标题。
- 长度上限不变:回合总结 120 字、候选摘要一行,不加长——塞实体是替换虚词,不是扩容。

## 候选只收四类

1. 用户明确说出的长期项目偏好(preference,confidence 用 user-stated)。
2. 已由源码、配置或工具结果核验的稳定项目事实(fact,confidence 用 verified,paths 至少给一项证据)。
3. 下个月再遇见仍能省一次排查的故障根因与验证办法(fact,confidence 用 verified)。
4. 用户当场明说的行事纠正(feedback,如版本节奏、验收习惯、提交规矩;confidence 必须用 user-stated)。用户没明说的,模型不得替他总结成 feedback——推断出来的 feedback 一律不收。

## 不收清单

任务进度、临时分支、端口号、PID、猜测(inferred 只在确有线索时给,且只进待审区)、整段聊天、个人资料、凭据、密钥、网页或 MCP 原文。拿不准就不收——宁缺毋滥。
