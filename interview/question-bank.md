# 高频技术面试追问题库

_面向 LubanCode 项目答辩：每题先给一口短答，再指出面试官真正想验什么与该翻哪处源码。_

---

[面试深挖导航](deep-dives.md) · [求职项目手册](portfolio.md) · [开发难题与故障复盘](retrospectives/development-challenges.md) · [模型、Provider 与 Schema 深挖](../docs/architecture/providers/schema.md) · [Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)

## 📋 怎么用这份题库

别背长答案。每题按四拍答：

1. 先定边界：这层管什么，不管什么。
2. 走成功路：给一件具体任务与数据形状。
3. 报失败路：哪一步会坏，坏后哪本账可信。
4. 讲取舍：为何这样做，现存欠账是什么。

若面试官不追，二十秒收口。若他往下钻，再报源码、测试与 corner case。

> 📌 **答题规矩:** 不会的边界直说“当前没做”。一个诚实欠账，常比一套空泛“高可用设计”更有分量。

## 🧠 模型、Provider 与 Schema

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 模型信息从哪来 | 内置 provider 快照、本地 `models.json`、端点 `/models` 各管一段 | 数据来源分层 |
| 参考了 OpenCode 什么 | 参考元数据驱动与覆盖思路，没照搬 schema；仓内直接记载借鉴 Codex catalog | 技术诚信、竞品研究 |
| JSON Schema 管什么 | 先分 provider catalog、tool input、structured output 三种 schema | 概念边界 |
| 为什么不全硬编码 | 新模型元数据可更新，wire adapter 仍守协议 | 数据与代码边界 |
| 为什么有 schema 还手写 parser | schema 给编辑器/CI，C++ 才是运行时校验与交叉引用 | 双契约维护 |
| capability 可信吗 | 是声明，不是探测；部分字段已接预算，部分只解析展示 | 能力协商 |
| 未知模型怎么办 | 允许直切，不猜高级能力，下一次请求验证 | 开放世界设计 |
| `/models` 与目录冲突信谁 | 端点列表判可用，目录补元数据 | 动态事实与静态知识 |
| variant 是新模型吗 | 不是；同一 model id 上叠请求参数 | 配置建模 |
| `extra_body` 如何合并 | 协议字段后叠 provider，再叠 variant；顶层浅合并 | 覆盖语义 |
| 输出上限从哪取 | config > provider > model catalog > unset | 优先级与 optional |
| 为什么 `unset` 不给默认数 | 服务端上限未知，擅猜会截短或拒绝请求 | 缺失值语义 |
| think 档位不认识怎么办 | 提示未经验证，不硬拦兼容端新值 | 前向兼容 |
| 新模型何时必须发版 | 只换 ID/元数据不用；新 wire 或新事件形状要改 adapter | 扩展边界 |
| 模型价格怎么算 | 当前没价格账，只记 usage；不能伪报金额 | 可观测性诚信 |
| 为什么模型专属 instruction 单独成段 | 不覆盖人格与环境段；切模型时能整体清掉 | prompt 组合 |

深读：[模型、Provider 与 JSON Schema 深挖](../docs/architecture/providers/schema.md)。

## 🔄 Agent Loop 与终止条件

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| turn、step、attempt 有何不同 | 外层任务、一次模型来回、一次 HTTP/SSE 尝试 | 状态机定义 |
| 主循环何时停 | 无工具的最终回答、取消、显式步数闸或输出预算耗尽 | 终止性 |
| `max_steps_per_turn=0` 是什么 | 无硬步数上限，不是零步 | 配置语义 |
| 如何防无限工具循环 | 可配步数闸、临近上限给收束提示、用户可 ESC | 失控保护 |
| 多工具为何顺序跑 | 守副作用、确认、Hook、转录与结果顺序 | 并发取舍 |
| stop reason 与内容冲突信谁 | 有 tool use 就按内容执行并配结果 | 防御性协议处理 |
| 工具失败是否抛异常 | 化成 `is_error=true` 的结果，交模型纠错 | 错误作为数据 |
| `max_tokens` 后为何还能继续 | 追加宿主标记开新 step，次数有界 | continuation 设计 |
| continuation 是重试吗 | 不是；输入与 step 都变了 | 术语准确 |
| 外来消息何时插入 | 工具收齐、下次请求未发的 step 边界 | 安全点 |
| 为何第一 step 不收 inbox | 刚提交的用户消息该先领起本 turn | 消息次序 |
| 工具表为何每步重建 | `tool_search` 可在中途挂新 schema | 动态能力 |

深读：[Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)。

## 🌐 三协议与流式解析

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 为何不为三家各写一套 Agent | wire 只翻请求与事件，主循环吃中立消息 | 抽象边界 |
| 中立层最小单位是什么 | Message、ContentBlock、StreamEvent | 数据模型 |
| tool call JSON 怎么拼完整 | input delta 累积，结束后再解析 object | 增量组装 |
| SSE 一帧跨多行怎么办 | framing 层按空行断 event，再拼多行 `data:` | 协议细节 |
| UTF-8 从半个字断开怎么办 | gate 暂存不完整尾巴，下一 delta 再拼 | 字节边界 |
| thinking 与正文怎样区分 | 归一成不同 block / delta，UI 与 history 分账 | 推理通道 |
| 三家 tool result 形状不同怎么办 | 中立 ToolResult，adapter 出线时再翻 | 反腐层 |
| Responses item id 有何用 | 维持后续关联与协议要求，不能随意丢 | wire 状态 |
| Anthropic 为何怕相邻 user | 角色交替更严，archive 要并进热区首条 user | 兼容约束 |
| Chat reasoning 要不要回放 | 按 provider 的 `reasoning_replay` 策略 | 供应商差异 |
| 流断了 partial assistant 怎么办 | 不冒充完整回复；ESC 路显式记打断，transport 错明报 | 一致性 |
| 如何加第四种协议 | 实现 request、events、client，再接 backend factory | 开闭原则 |

深读：[Query 数据流与三协议适配](../docs/architecture/query-data-flow.md)。

## 🔧 工具调用与 JSON

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 单工具 wire 上是什么 | provider 原生 tool/function JSON；宿主内统一 ToolUseBlock | 内外数据形状 |
| 多工具结果怎么回 | 顺序执行，结果按调用 id 同批回填 user message | 配对协议 |
| tool call id 谁生成 | 通常 provider 给；宿主按 id 关联 result | 关联键 |
| 参数坏 JSON 怎么办 | assembler / adapter 报解析错，不拿半成品执行 | 输入完整性 |
| JSON Schema 是否统一验原始参数 | 当前尚未全统一；工具 execute 仍自验 | 现存欠账 |
| Hook 改参后为何再验 schema | 防 Hook 绕过工具参数边界 | 变更后验证 |
| Schema 支持多深 | 当前宿主轻量校验只做一层基础类型、required、enum | 不能说满 |
| 未知工具怎么办 | 回错误 ToolResult，不崩主循环 | 开放集合 |
| 工具说明为何也占 context | name、description、schema 都在请求前缀 | 隐性成本 |
| 延迟工具怎么找 | 先注索引，模型调 `tool_search`，下一 step 挂完整 schema | 按需加载 |
| 工具返回很长怎么办 | 源头封顶，再 artifact + 预览 + microcompact | 长结果治理 |
| side-effect 工具为何不判重 | stdout 相同不代表动作相同 | 幂等判断 |

深读：[工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[工具调用流程](../docs/architecture/tool-calling-flow.md)。

## 🔐 权限、安全与注入

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 模型说“这是安全命令”能信吗 | 不能；确认、策略、Hook 与 OS 权限在宿主 | 信任边界 |
| Prompt injection 怎么挡 | 工具输出与外来资料标不可信，权限不交给模型 | 指令层级 |
| `read_file` 能读哪里 | 按工具路径规则与项目边界，不把 schema 当权限 | 文件隔离 |
| 命令为何要确认 | shell 是高副作用能力，参数形状合法不等于安全 | capability security |
| allow 与 deny 冲突谁赢 | deny 优先，固定归并次序 | 策略确定性 |
| 多 Hook 同时表态怎么办 | 同步并发执行，定义顺序归并，权限 `deny > ask > allow` | 并发归并 |
| PostToolUse 能回滚吗 | 不能；副作用已发生，只能报错与留痕 | 事务边界 |
| MCP 工具为何默认确认 | 外部服务器实现不在本仓信任域 | 第三方代码 |
| API key 会不会进 catalog | 不进；只存 env 名或发前替换占位符 | 密钥治理 |
| extra headers 能覆盖认证吗 | 能，空值还能删；高级配置者自负其责 | 逃生口风险 |
| 在线 catalog 安全吗 | HTTPS、大小、schema、revision、原子写；尚无内容签名 | 供应链 |
| 项目配置能否偷偷开记忆 | 不能，只能在全局授权内降档 | 陌生仓库边界 |

深读：[安全模型](../docs/development/security.md)、[Hooks 流程](../docs/architecture/hooks-flow.md)。

## 📚 Context、长文本与 compact

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 为什么不把历史全塞回去 | 窗口、成本、缓存与注意力都会坏 | 预算意识 |
| history、request view、session 有何不同 | 活历史、临时请求视图、完整持久流水 | 状态分账 |
| system prompt 会被 compact 吗 | 不会；compact 有自己的 system，只换 history | prompt 稳定 |
| 保留最近几条 | L3 按完整 turn 与 token；L4 才是首轮加最近三轮 | 算法准确 |
| 程序过滤与 LLM 总结如何分工 | 程序守边界与引用，LLM 做语义归纳 | 混合算法 |
| episode 在哪切 | 新用户输入与 `todo_write` | 任务阶段识别 |
| map/reduce 为什么要多层 | compact 模型自己也有窗口 | 二阶窗口问题 |
| 摘要漏待办怎么办 | manifest 与 active todo 守恒不过便拒收 | 可验证摘要 |
| compact 失败怎么办 | history 不动，L1/L4 继续兜底 | 失败原子性 |
| 单条用户输入超窗怎么办 | 无冷区可切，明确拒绝，让用户落文件或拆分 | 不可能边界 |
| hard trim 有损吗 | 有；只改请求视图，终端告警，session 原文留着 | 降级可见 |
| prompt cache 如何稳 | system 稳定、L1 决策钉住、hard trim sticky、epoch 明记断因 | 缓存工程 |

深读：[Context 压缩算法深挖](../docs/architecture/context/compaction.md)。

## 💾 Session、重试与恢复

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 网络错会自动重试吗 | 当前不会；一 step 一 attempt | 副作用意识 |
| 为什么不重试 `429/5xx` | 流可能已有正文/tool use，工具可能已执行 | exactly-once 困难 |
| 崩溃会丢整场吗 | JSONL 逐条 append+flush，通常只伤尾行 | 持久化模型 |
| session 首行坏怎么办 | meta 无法识别，整场不能 resume | 根记录重要 |
| 中间一行坏怎么办 | 跳过并计数，继续回放完整行 | 部分恢复 |
| tool use 缺 result 怎么办 | 补显式错误结果，修协议形状 | 不变量修补 |
| 孤儿 result 怎么办 | 找不到任何 use 便删除 | 引用完整性 |
| compact marker 如何回放 | 读到事件便替换有效 history，旧消息仍在 all_messages | 事件溯源 |
| marker 写失败怎么办 | 内存已短、磁盘仍长；重启回到压缩前 | 两阶段裂口 |
| ESC 后保存什么 | partial assistant、打断标记、已完成结果与未执行错误结果 | 取消语义 |
| memory job 会重试吗 | pending job 可跨进程再捞，坏 job 进 failed | 耐久队列 |
| MCP 超时后迟到包怎么办 | pending id 已删，迟到响应丢弃 | late response |

深读：[Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)。

## ⚙️ 并发、取消与后台任务

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 哪些东西并发 | 流式输入监听、footer 心跳、Hook handler、子代理等各有边界 | 并发地图 |
| 主工具为何不并发 | 副作用与确认次序优先 | 串并取舍 |
| cancel flag 如何跨线程 | `atomic<bool>`，监听线程写，执行线程读 | 内存可见性 |
| 为何不用普通 bool | 跨线程读写是 data race | C++ 内存模型 |
| 取消命令怎么杀树 | Windows Job Object，POSIX 进程组 | OS 差异 |
| 后台任务跨重启吗 | 不跨；日志可留，内存 task registry 不重建 | 持久性边界 |
| 子代理结果何时进主循环 | 安全点 drain completion notices | 消息交接 |
| 子代理能否污染主 history | 主 history 只收委托与最终结果，中间探索隔离 | 上下文隔离 |
| MCP 进程死了会自启吗 | 当前不会；重启还须重新 initialize 与 tools/list | 生命周期 |
| Hook 并发返回如何稳定 | 收齐后按定义次序归并，不按完成先后 | 确定性 |

深读：[文件读取与命令执行深挖](../docs/architecture/tools/file-commands.md)、[Query 数据流里的子代理路径](../docs/architecture/query-data-flow.md#agent-子代理为何不一样)。

## 🌐 MCP、Skill、Hook 与插件

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| MCP 是不是 HTTP | 当前 stdio 长命子进程，一行一帧 JSON-RPC | 协议实现 |
| MCP 怎样握手 | initialize、initialized notification、tools/list | 生命周期 |
| stdout 能混日志吗 | 不能；一行须是协议 JSON，日志走 stderr | framing 边界 |
| MCP content 都支持吗 | 当前主要拼 text，其他类型有占位 | 不能说满 |
| Skill 如何触发 | 常驻名称摘要，命中后模型调 `skill` 按名加载全文 | 渐进披露 |
| Skill 是代码吗 | 主要是说明与流程，不直接绕过普通工具权限 | 能力边界 |
| 同名 Skill 谁赢 | 按本地覆盖层级决定 | 发现顺序 |
| Hook 与 Skill 差别 | Skill 教模型，Hook 由宿主在事件边界直接执行 | 控制面 |
| Hook async 做了吗 | 当前解析展示并记 skipped，没真异步跑 | 实现现状 |
| 插件到底有什么用 | 让可信本地函数不必包装成 MCP 服务 | 设计动机 |
| 插件如何进工具表 | Lua/C 定义适配成统一 Tool 接口与 Schema | 扩展接口 |
| 插件能真装吗 | Lua 拷文件；Windows C 插件编 DLL 再拷；重启后 `/plugins` 查 | 可交付性 |
| 为什么不用全用 MCP | MCP 换隔离与独立生命周期；插件换少部署与低调用成本 | 取舍能力 |
| 为什么 Lua 与 C 都留 | Lua 轻，C 能接原生库；开发门槛与能力范围不同 | 分层设计 |
| C ABI 为何不用 C++ 类 | 收窄编译器、标准库、对象布局与跨 CRT 边界 | ABI 意识 |
| 谁分配谁释放怎么做 | DLL 返回 content，宿主拷贝后回调插件 free_result | 内存边界 |
| 插件调用前确认就安全吗 | 不；Lua 顶层与 DllMain 在加载时已能执行 | 信任边界 |
| Lua/DLL 崩了怎么办 | 普通 Lua error 可接；死循环、os.exit、DLL 崩溃会拖宿主 | 不能说满 |
| 插件有热更新和包管理吗 | 当前都没有；投放文件后重启 | 产品现状 |
| 插件并发安全吗 | DLL 作者自保；Lua 同 state 加锁串行，不同 state 可并行 | 并发边界 |
| 扩展工具为何还要确认 | 注册成功不等于可信 | 最小信任 |

深读：[工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[进程内插件系统深挖](../docs/architecture/extensions/plugin-runtime.md)。

## 📈 性能、缓存与成本

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 最大 context 就一定更好吗 | 不；日志噪声、延迟、价格与注意力都有成本 | 性能观 |
| token 怎么估 | 本地启发式用于预判，provider usage 用于实账 | 估算与测量 |
| 为什么还留字符硬限 | token 估算与窗口资料可能不准 | 双保险 |
| tool schema 占多少 | 名、说明、JSON Schema 全算；工具多时延迟挂载 | 静态开销 |
| prompt cache 何时失效 | system、旧消息、工具定义或裁剪形状变化 | 前缀稳定 |
| 为什么记 cache epoch | 把“为何断缓存”从猜测变成可观察事件 | 可诊断性 |
| artifact 会不会无限长 | 需靠会话仓与清理策略；请求只留预览 | 存储预算 |
| 命令输出为何 2 MiB 封顶 | 防失控进程吃内存与 context | 资源限制 |
| microcompact 为何有迟滞 | 冷区不长 50% 不再跑，防反复烧模型 | 抖动控制 |
| usage 缺失怎么计成本 | 明写未报告，不当零 | 数据质量 |
| cheap 模型失败会怎样 | 后台小活降级，不拖垮 normal turn | 服务分级 |
| 为何不用向量库做 memory | 当前 BM25 更轻、本地、可解释；召回语义较弱 | 取舍能力 |

深读：[上下文、长文本与记忆深挖](../docs/architecture/memory/context.md)。

## 🧪 测试、可维护性与跨平台

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 网络协议怎么测 | parser 与 request builder 先做纯函数测试，client 再测边界 | 可测试设计 |
| 为什么大量 pure function | 输入输出可钉，少靠真实终端与网络 | 确定性 |
| SSE 怎么做失败测试 | 喂碎帧、坏 JSON、半 UTF-8、错 stop reason | 边界覆盖 |
| session 恢复测什么 | 坏行、缺 result、重复 compact、最后事件胜出 | 恢复正确性 |
| schema 改字段如何防漏 | schema、C++ allowlist、parser 与测试同改 | 契约同步 |
| Windows 与 POSIX 最大差别 | 路径编码、Job Object/进程组、终端重画 | 平台抽象 |
| 为什么 C++23 | expected、variant、性能与单二进制；代价是异步和生态更重 | 技术选型 |
| 如何查内存泄漏 | RAII、进程句柄封装、长会话压力与 sanitizer | 资源生命周期 |
| 文档如何防过期 | 行为页连源码入口，变更矩阵与测试一同审 | 文档工程 |
| 如何做回归演示 | 固定 Release 包、假 provider/fixture、成功与失败各一条 | 可复现性 |
| 哪处最难维护 | 协议兼容差异与终端并发，需窄接口和纯函数 | 复杂度识别 |
| 下一步先重构哪里 | 选有证据的欠账，如 schema 单源生成或 capability gate | 优先级判断 |

深读：[架构说明](../docs/architecture/README.md)、[文档规范](../docs/development/documentation.md)。

## 💥 开发难题与故障复盘

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 开发中最难的问题是什么 | 先挑一件跨边界故障，不要说“时间紧、任务重” | 技术深度 |
| 遇过最严重的线上事故吗 | 报影响面、止血、根因、修复与防复发 | 事故担当 |
| 哪个 bug 最难复现 | 讲并发请求永挂；用裸 socket 复刻“连接成、响应不来” | 复现能力 |
| 用户现场复现不了怎么办 | 留版本、平台、输入、日志与时间线，再缩变量 | 取证习惯 |
| 有过误判吗 | Unicode 故障先疑 wire，后来又查到 `path::string()` 窄口 | 诚实与排除法 |
| 怎样证明找到了根因 | 修前测试必红，修后必绿；解释因果，不只报相关性 | 证据强度 |
| 为什么同一问题修三层 | 源头保真、统一边界保命、旧数据入口兜底 | 分层设计 |
| 测试全绿为何用户还能撞 bug | fixture 没复刻故障形状，或只跑了 POSIX 语义 | 测试反思 |
| 跨平台最坑的地方 | 文件柄、ACP/UTF-8、控制台与进程组语义各不相同 | 平台经验 |
| 遇过内存生命周期问题吗 | lambda 引用退栈对象，ASAN 抓到 stack-use-after-scope | C++ 基础 |
| 遇过资源泄漏或退出挂死吗 | 后台请求永挂加无界 join，须给请求与退出各设边界 | 生命周期治理 |
| 怎样设计故障回归 | 不测抽象“超时”；真起装死 socket，不回响应头 | 测试质量 |
| 修完 bug 还要做什么 | 补 fixture、注释、错误文案、排障文档与回归 | 工程闭环 |
| 现在还有什么已知问题 | 排队消息尚无 durable ack，失败与重启可能丢 | 边界诚实 |
| 若重做一次先改什么 | 先把 queue 变成 pending/inflight/acked 的耐久协议 | 优先级判断 |

深读：[开发难题与故障复盘](retrospectives/development-challenges.md)。每件事都照“现场 → 误判 → 根因 → 修法 → 验收”拆开。

## 🎓 项目归属与设计取舍

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 哪些是你独立设计 | 报具体边界、提交与测试，不报空泛“全栈” | 真实贡献 |
| 参考开源是否算抄 | 说明看过什么、重写什么、为何不同、许可证如何处理 | 工程伦理 |
| 为什么不用现成 SDK | 三协议统一、C++ 运行时与细粒度流事件需要自控 | build vs buy |
| 最大一次事故是什么 | 讲症状、根因、不变量、回归测试 | 故障复盘 |
| 最后悔的设计是什么 | 选真实欠账，讲迁移路，不要假装完美 | 反思能力 |
| 若用户量涨百倍怎么办 | CLI 本地瓶颈与云服务瓶颈分开，不乱套分布式 | 场景判断 |
| 哪项指标最该加 | attempt、首字节、cache hit、compact 收益、tool latency | 可观测性 |
| 如何排技术债 | 看事故频率、数据损失风险、共享边界与测试成本 | 工程判断 |
| 为什么项目值得做 | 协议、工具、终端与恢复在一只真实 runtime 里相互制约 | 项目价值 |
| 与 OpenCode/Codex 区别 | 报语言、部署、支持边界与设计取舍，不贬竞品 | 竞品理解 |

项目故事与简历说法见[求职项目手册](portfolio.md)。真实故障怎样讲，见[开发难题与故障复盘](retrospectives/development-challenges.md)。

## ⚠️ 十句不能说满的话

1. 不说“支持所有 OpenAI 兼容端”。兼容端常改字段、路径与 SSE。
2. 不说“接入了 OpenCode/Models.dev”。当前只是同类思路对照，格式不兼容。
3. 不说“JSON Schema 保证安全”。它只管形状的一部分。
4. 不说“模型 capability 全部自动生效”。若干字段仍只解析存储。
5. 不说“模型请求会自动重试”。当前主请求没有。
6. 不说“多工具并行”。主 AgentLoop 顺序执行。
7. 不说“compact 绝不丢信息”。机器只强验 manifest 与活动待办。
8. 不说“session 绝不会丢”。首行坏、落盘失败与磁盘故障仍有边界。
9. 不说“MCP 全协议支持”。当前 transport 与 content 类型都有范围。
10. 不说“跨平台语义完全一致”。Windows 与 POSIX 的进程、终端能力不同。

## 🔗 最后复习路线

时间只剩半小时，按这次序读：

1. [面试深挖导航](deep-dives.md)
2. [开发难题与故障复盘](retrospectives/development-challenges.md)
3. [Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)
4. [模型、Provider 与 JSON Schema 深挖](../docs/architecture/providers/schema.md)
5. [Context 压缩算法深挖](../docs/architecture/context/compaction.md)
6. [工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)
7. [求职项目手册](portfolio.md)

每页挑一条成功路、一条失败路、一项欠账。把源码入口记住。面试官再往下问，便有路可走。
