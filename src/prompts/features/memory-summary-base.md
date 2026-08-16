# 回合总结与记忆候选提取

你拿到的是一段终端 AI 编程工具里刚结束的一个回合(用户消息、助手回答、工具调用摘要)。请先在心里推测用户这一回合的目的,再按下面分型提示词的侧重,产出一回合总结,并提取值得长期记住的候选。

## 输出格式(严格遵守)

只输出一个 JSON object,不加代码围栏、不加任何解释文字:

```
{"task_type":"code|research|config|docs|other","summary":"不超过 120 字的回合总结","retrieval_terms":["下一轮检索用的关键词或同义改写,最多 8 个"],"candidates":[{"kind":"fact|preference|feedback","title":"短主题","summary":"一行摘要","content":"精炼正文,含 ## Why 小节写来龙去脉(没有根据就省略,不编故事)","keywords":["精确检索词"],"paths":["支撑证据的项目内相对路径"],"confidence":"user-stated|verified|inferred"}]}
```

candidates 可以为空数组,最多 3 条。retrieval_terms 是给下一轮记忆检索用的扩展词:同义词、更标准的叫法、涉及的符号名/路径,不要放整句话。

## 候选只收四类

1. 用户明确说出的长期项目偏好(preference,confidence 用 user-stated)。
2. 已由源码、配置或工具结果核验的稳定项目事实(fact,confidence 用 verified,paths 至少给一项证据)。
3. 下个月再遇见仍能省一次排查的故障根因与验证办法(fact,confidence 用 verified)。
4. 用户当场明说的行事纠正(feedback,如版本节奏、验收习惯、提交规矩;confidence 必须用 user-stated)。用户没明说的,模型不得替他总结成 feedback——推断出来的 feedback 一律不收。

## 不收清单

任务进度、临时分支、端口号、PID、猜测(inferred 只在确有线索时给,且只进待审区)、整段聊天、个人资料、凭据、密钥、网页或 MCP 原文。拿不准就不收——宁缺毋滥。
