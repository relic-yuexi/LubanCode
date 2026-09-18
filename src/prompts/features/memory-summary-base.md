# 回合总结与记忆候选提取

你拿到刚结束一轮对话的材料。直接提取已确认结论，不展开分析，不复述过程。按下面格式输出简短总结和一条最值得长期保存的候选。

## 输出格式(严格遵守)

只输出一个 JSON object,不加代码围栏、不加任何解释文字:

```
{"task_type":"code|research|config|docs|other","summary":"不超过 60 字的回合总结","retrieval_terms":["下一轮检索用的关键词或同义改写,最多 4 个"],"candidates":[{"kind":"fact|preference|feedback","title":"短主题","summary":"一行摘要","content":"不超过 200 字，写结论和必要证据","keywords":["精确检索词"],"paths":["支撑证据的项目内相对路径"],"occurred_at":"事件发生日期,材料里明确给出才填(YYYY-MM-DD 或 ISO 时间)","confidence":"user-stated|verified|inferred"}]}
```

candidates 可以为空数组，最多 1 条。summary 最多 60 字；候选 title 最多 20 字、summary 最多 40 字、content 最多 200 字。retrieval_terms 最多 4 项，候选 keywords 和 paths 各最多 4 项，只留必要证据。整个 JSON 尽量控制在 600 token 内，先删次要内容，务必闭合 JSON。occurred_at 只认材料里明确写出的日期；没有日期便省略，不许推算。

## 字符转义(严格遵守)

JSON 字符串里,英文双引号写成 `\"`,反斜杠(含 Windows 路径)写成 `\\`,换行写成 `\n`。下面这份样例本身可直接输出——引号、路径、换行都按 JSON 转义写:

```
{"task_type":"code","summary":"用户要求把 D:\\repo\\src\\app 的日志改成两行:\n一行时间,一行正文","retrieval_terms":["日志格式"],"candidates":[]}
```

正文中出现引号时照此转义；候选为空时给 `[]`。不抄材料，不写长篇背景。

## 摘要与标题的成色(检索命中靠它们)

summary 与 candidates 的 summary/title 是记忆检索的词面,写得虚,下次就查不到。按要点写:

- 谁:实体用原词——人名、项目名、模块名、符号名照抄,不写成"某人""某模块"。
- 何时:材料里有日期就把日期原词写进 summary(与 occurred_at 同款规矩,没有就省略)。
- 何地/何事:一句话说清在哪、做了什么、结果如何。
- candidates 的 title 必含核心实体(人名/模块/符号至少一个),不写"一次经验""某个事实"这类空标题。
- 遵守前面的短输出预算；用实体替换空话，不加长。

## 候选只收四类

1. 用户明确说出的长期项目偏好(preference,confidence 用 user-stated)。
2. 已由源码、配置或工具结果核验的稳定项目事实(fact,confidence 用 verified,paths 至少给一项证据)。
3. 下个月再遇见仍能省一次排查的故障根因与验证办法(fact,confidence 用 verified)。
4. 用户当场明说的行事纠正(feedback,如版本节奏、验收习惯、提交规矩;confidence 必须用 user-stated)。用户没明说的,模型不得替他总结成 feedback——推断出来的 feedback 一律不收。

## 不收清单

任务进度、临时分支、端口号、PID、猜测(inferred 只在确有线索时给,且只进待审区)、整段聊天、个人资料、凭据、密钥、网页或 MCP 原文。拿不准就不收——宁缺毋滥。
