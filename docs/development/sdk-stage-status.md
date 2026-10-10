# SDK 阶段进度与验收边界

八步总 goal 保持 active。第一步基线已验收；第二步尚未完成，第三至八步
沿原清单推进，不用当前用量补口替整项验收。不跑本地 CI，三平台证据
必须来自同一提交；已合分支核实后清理，在用分支和恢复备份保留。

`d3d2c0e7` 的 TSan 编译另报 doctest 复合断言错误，定位到 assembler
词法见证夹具。候选仅给原判断加括号，原排除值不变。四家 parser 退栈
夹具另显式包含 array、functional。这些修补均待新源远端编译与实跑。

`d3d2c0e7` 的 macOS 安装消费者给出差账原数：父场实有两笔，五项为
`2,2,6,10,14`；子场预期三笔，父报告实有零笔、五项全零。SDK 派工
Hooks 未绑定本轮 `TurnEventAdapter`，子场自身有 owner，父场却未接观察。
候选已绑当前前台事件 owner，由原 ChildTurnScope 在退场前清借用。原三笔
断言不减；新源三平台、采用材料、恢复与关场仍待实跑，未记修复通过。

连接异常数字出口候选现贯穿四家 parser/client：规范快照一旦返回 parser，
便把五项存入固定槽；材料或事件列表随后抛错，请求退栈时尽力交数字。
回调正常接过便清待交位；宿主回调自身抛错不重投。次生回调错误不遮原错。
恢复只交数字，响应号与原材料保持未知，不造正文、成功终帧或耐久 ACK。
二十四条四家实际 parser 源夹具核标准、分配、未知异常，已交与未交分支，
以及次生回调故障。尚未执行原生。规范快照生成前的词法、原数或规范化
分配故障，及 Responses 非流式批量展开，还须补 owner；此处不冒称全链已齐。

`66647cb2` 远端已报错：Windows 的 `min/max` 宏撞上新增整数检查；Linux
SDK 已完成构建，安装消费三十七场过三十六场，子 Agent 五项汇总断言失败。
本地仅改源码：给九处 limits 调用加括号，补公开子场实际样本数和五项数字诊断。
断言不减、不改预期；子场差账根因还未查清。新修正及关场夹具尚未推送验收。

关场源夹具新增公开 SDK 的迟到回复：真实 `Session::Close()` 升取消旗后，
宿主 Backend 仍卡在门内。Close 不得先回、不准先析构。放开门，回调交五项
数字、原材料与响应号，才退出；终态仍取消，不交迟到正文。Closed 读面及
同 ID 恢复再核单笔用量、单条来源记录和零模型重放。生命周期名册增至二十案，
旧十九案保留；远端门另核 callback、cancellation、retired、recovery 四个实跑标记。
夹具与门均未执行，本处不算关场验收。当前已推 `66647cb2` 的 CI 仍在编译。

材料准入候选补已捕获数字出口：Builder 和 Anthropic 累积视图校验失败时，
保数字，另标材料错误；parser 沿原数字事件写口交事实，再关错误终态。
拒收材料不进入公开观察，不交成功终帧，不给精确覆盖。容量源夹具补 raw、
异常和路径三道帽，核 assembler 留五项及真实响应号，覆盖账仍未知。
尚未执行原生；这处不包揽数字捕获前或事件交付前的分配故障。

任务显示候选补三份真实投影：Outcome、Snapshot、Summary 的完整输入与总量
均逐步检查 int64。面板、完成通知、后台详情与预算文案遇溢出显示 `?`，
不截零、不饱和成假数；原始五项保持原值，reasoning 已包含在输出中，不再加。
补正溢出、负下溢、输出单独溢出、明确零及 reasoning 不重复计数的源夹具。
原生尚未执行；其它旧 `api::TotalInputTokens` 消费口仍须逐处核，不能称全链已齐。

2026-10-11 五项用量候选已提交为 `e7db494f`，开成草稿
[PR #349](https://github.com/relic-yuexi/LubanCode/pull/349)。同源
[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/38070702028)
仍在运行；三套 SDK 腿先停在安装头文件数据夹具。生产检查新增 `usage.hpp`，
夹具模拟目录却少了它，报三十四次断言失败、两次错误。编译、原生验收和
安装消费尚未执行，不能算 SDK 通过。修正只补夹具头文件，并把这枚头加入
删源不能缩小安装合同的原负例。修正尚无新源远端验收。

已合并用量快照远端分支 `codex/sdk-memory-usage-snapshot` 已按旧 SHA
`35468c7f` 的 lease 删除，GitHub ref 查询确认不存在。本地恢复 bundle 保留。

2026-10-11 连接故障候选：SSE 写回调收住 parser 和宿主抛错，断流后等
`Post()` 退出，再交回原异常。取消旗不能遮掉原错；错误响应体也改用差值
检查容量，免得长度相加溢出。四家真实 Connection 共添二十条故障路径：
先收到真实用量，再报流内错、取消，或抛标准、分配、未知异常。CLI 与 SDK
共享夹具，核五项数字、响应号、原材料、失败归因、单次调用及服务线程退出。
这些均是未执行源码，不能算原生通过。

远端 SDK Memory 名册增至十一案，采样名册增至二十案，旧案名字保留。
ASan 登记补四家 parser、流传输、Loop、Agent tool、V3 写账、schema 和
Memory 旁路；单改 wire 源码也须触发三平台 SDK 与 ASan。还欠 parser 交出
事件前遇分配故障的数字归属、完整关场、安装消费与同源远端验收。
本处只留已送达采样 owner 的数字，不承诺所有 wire 分配故障均能保住事实。
八步总 goal 保持进行中。

2026-10-10 当前用量候选：Operation 已接逐请求记录，留五项数字、原始材料、
真实请求与响应身份、模型、step、turn、用途及冻结 cache epoch。结果格式 v2
保存记录与汇总；完整记录恢复后重算两册汇总，不符就拒收。旧 v1 仍可读取，
记录完整度保持未知。兼容事件缺少真实 typed owner，不能把记录标成完整。
每场最多留 1024 条记录。下一条先归拢数字，再停本轮、关闭精确判定；
材料准入或复制失败也照此留汇总，不继续生产无处保存的记录。

这批仍是未提交源码。新增容量、同 ID 恢复和坏材料夹具尚未执行；没有借旧 CI
验收。记录还未逐条核回真实主账，也未承诺每次请求结束即耐久保存。
真实连接故障、关场、完整安装消费及三平台远端验收仍欠。八步总 goal 保持进行中。

子场台账候选现接五项检查累加与逐字段覆盖账。真实模型请求先归拢数字，
再走事件复制和父场回调；父场转发回执只认父 owner 真正接过材料。
Snapshot、Outcome 和轻量列表均留 reasoning 与覆盖账。原两请求案例保留，
另添 reasoning 溢出场，核该项保前缀、其余四项照常累加。尚未执行原生验收；
旧显示口拼完整输入与总量仍须查溢出，不能拿本处修正冒称全链已齐。

主场逐请求记录候选再加来源核验：写最终结果与同 ID 恢复都拿已验主账，
逐条核 request、Session、run、turn、模型、生产方 step、请求用途和 cache epoch，
并核 prepared 输入链。运行 step 与主账 step 分开留值，不假称同一枚号。
V3 原有 `conversation` 用途照留，另加 `requestPurpose` 和 `producerStepId`。
错绑不记已完成；恢复拒收。六种串场夹具已写，尚未跑。子场采用账、响应身份
和原材料同主账逐值核对仍欠；这笔不冒充完整来源验收。

子场来源前置再补 Session/run 两枚身份。Loop 只从真实 boundary writer
借值，旧替身与未绑主账留空，不造身份。子场 Drive 开始前绑定实际 child turn，
不再把空 turn 混入请求记录。结果保存与恢复保这两枚值；主场错 Session/run
拒收。安装消费者补逐请求数、真实子场身份与用途检查，同 ID 恢复仍核零模型
重放。源码尚未跑远端；父场采用记录到子账的逐请求核验还未接齐。

子场请求来源候选现接到实际采用检查。借同一次有界读取，先核子账终态、
父场来源、结果选择与输入采用，再核逐请求 Session/run、request、turn、模型、
生产方 step、用途、cache epoch 和 prepared 输入链。重复请求与无主记录拒收。
父场尚未消费子结果仍保 Incomplete；借读面不升级采用结论。默认关子 Agent
的场拒收 subordinate 记录。八种真实同 ID 恢复串场夹具已写，核拒收、原件
不改与零模型／副作用重放。来源借用原生夹具也已补；均未跑远端验收。
原材料与真实响应身份逐值持久核对、连接故障、关场及整套 CI 仍欠。

响应身份候选修正旧起帧取值：公开报告、主账收口与工具追踪共取 assembler
最后实报 ID，缺号仍留空。正式 assistant 正文另留 `provider_response_id`，
不拿本地 request 号代填。已落正式响应时，恢复再核这枚 ID 与五项用量。
四种起帧／后报／缺号组合、真实 SDK 主账五项原件及错响应恢复夹具已补，
尚未执行。失败与取消若没有正式 assistant，仍只核已有 prepared 来源，
不宣称已有完整响应证据。原始观察独立落账、responseModel 旧映射与整套
连接／关场验收继续欠账，不能凭本处修正升成完整来源交付。

2026-10-11 原始观察候选已接主场与前台子场的 V3 写口。数字先入采样 owner，
再追加独立 `model.usage.observed`，留五项原数、真实响应号、明报位、不完整位
与有界原材料。观察不另收费，不代替 assistant、采用结论或失败持久 ACK。
支持写口报失败就停本轮；旧替身和 V2 明记不支持。传输抛错或数字 owner
抛错时，仍尽力落观察，不让次生错误遮掉原错。此处尚未证明所有分配故障可落账。
恢复逐项核观察与结果记录，不能把改过原材料当旧事实。主场和 Memory 旁路
的响应号已与 responseModel 分开，未观察到模型名记 null。原件读取、坏材料
恢复与旁路来源夹具已补，均未跑远端。schema 故障矩阵、完整连接／关场
和三平台验收仍欠；八步总目标保持进行中。

2026-10-11 Memory 采样旁路候选也接原始观察。五项数字先入 SampleResult，
再借真实 request、旁路 turn 和 step 落观察，随后才拼归因文案、复制材料与正文。
主场与旁路共用有界材料构造；写口、耐久提交和通知各归原桥。观察写失败或
抛错不能走成功出口；已有传输或取消错误先返回。没有回调、身份或用量事实
的纯超时不造观察。旧写口明记不支持，照留原行为。主 Loop 也补旧 Backend
在失败前报非零数字的来源判定，不拿缺报位抹掉数字。
成功旁路、纯超时及写口拒绝／未知异常夹具已写，未执行。这里不承诺完整
采样恢复，也不承诺后续材料分配失败仍能返回 SampleResult；失败 ACK、
schema 矩阵、完整连接与关场、安装消费和三平台远端验收仍欠。

原始观察候选再补跨行来源约束。实际写口、验卷共用检查；普通续卷和拥有
捕获材料的续卷都恢复该状态。观察前须有同请求 prepared、sent；四枚归属
身份相同，每次物理请求只收一条。旧账无观察不补造。追加后内存更新失败
沿原 Journal lease 留真实落盘回执、断开句柄，恢复也不能重复写已落观察。
15 种形状／坏材料、四种重算哈希串场／重复／缺边界、两类实际 writer
提交故障夹具已写，尚未执行。Python 校验器登记新事件并核形状、材料数量
及来源序列；材料操作数与选中数字的完整语义仍由原生 Decode 核。
这轮只交源码候选，不算验收；完整连接、关场与全套远端证据仍欠。

Memory 抽取候选改用公共五项检查累加，逐字段覆盖账相随；沿旧口保最近
一次 reported 标志与时长，不拿它判精确。共享 CLI／SDK 抽取夹具补 reasoning
越界与失败后继续出账，越界项留前缀，其余四项照加。实际 HTTP Connection
夹具从 Anthropic、Responses 扩到 Chat、Gemini；各核数字、真实响应号与
原材料，服务线程收工后核四次 POST。均未执行，完整连接故障、取消与关场
矩阵仍欠，不能拿新增成功场冒称四家全链验收。

输入校准候选改用逐字段精确门。输入、缓存读、缓存写均有有效来源、没有
推断、异常或累加缺口，才取检查相加总数；旧 usage 明报位不能越门。明报零
照收。输出域单独异常不抹输入域事实。汇总按原数补负数异常，宿主漏写标签
也不能把负数标精确。实际 Loop 七种来源与独立输出异常／漏标签负数夹具
已写，尚未执行。安装检查另钉新公开 usage 头；SDK 文档写明未知、容量与
恢复边界。自动 Memory owner、完整连接故障和关场矩阵及三平台验收仍欠。

2026-10-09 续批：第 2 步[显式 web_search](sdk-web-search.md) 已随
[#335](https://github.com/relic-yuexi/LubanCode/pull/335) 合入 `194f587b`，与验收源
`d596e8e0` 同树。远端全量 Windows 791、Linux 789、macOS 792 项均过；
三平台独立 SDK Lua ON/OFF 各 70/68 项均过，九册安装消费各 15 次真实 HTTP
均过；ASan 169 项过，Workflow TSan 本片未触发。74 册产物及原日志已封存。
[SDK 开场线程失败清理](sdk-opening-start-cleanup.md) 已随
[#336](https://github.com/relic-yuexi/LubanCode/pull/336) 合入 `8a19edf4`，与验收源
`49060155` 同树。全量 Windows 791、Linux 789、macOS 792 项均过，三平台
测试名单与 #335 相同；独立 SDK ON/OFF 各 70/68 项、生命周期 12 案及五条
真实失败路径、九套安装消费均过。ASan 170 来源、1881 CASE、77461 断言过，
745 份源码及输入哈希与实际 Git 对象吻合；74 册产物已封存。未跑本地 CI。
[采样参数与终止原因](sdk-model-sampling.md) 已随
[#337](https://github.com/relic-yuexi/LubanCode/pull/337) 合入 `adab6fb3`，与验收源
`108eed10` 同树。全量 Windows 793、Linux 791、macOS 794 项均过；只增两条
采样登记，旧案例未减。独立 SDK ON/OFF 各 71/69 来源、九套安装消费各 37/34
案例均过，实际 DLL 拒绝非法终止原因。三平台真实 Memory 抽取核过三种截断。
ASan 171 来源、1887 CASE、77578 断言过；746 份输入哈希与 Git 对象吻合。
74 册产物及 1925 份索引原件已封存，未跑本地 CI。
[旁路采样看门狗关场](sdk-sampling-lifetime.md) 已随
[#338](https://github.com/relic-yuexi/LubanCode/pull/338) 合入 `068db5d3`，与验收源
`639a9dc2` 同树。全量 Windows 795、Linux 793、macOS 796 项均过，旧名单未减。
三平台 SDK ON/OFF 各 72/70 来源、九套安装消费各 37/34 项及各 15 次真实 HTTP
均过；SDK DLL 内六条故障路径与 CLI 采样 25 案均过。ASan 172 来源、1899 CASE、
78519 断言过；747 份测试源码、附件及配置输入与 Git 原件吻合。77 册产物、1941 份索引原件
逐大小和 SHA256 封存。TSan 本片跳过，不计通过。未跑本地 CI。
[Memory 抽取共享底座](sdk-memory-extraction-core.md) 已随
[#339](https://github.com/relic-yuexi/LubanCode/pull/339) 合入 `52b71aa8`，与验收源
`5c463e9b` 同树。Windows/Linux/macOS 全 CLI 各 798/796/799 项均过，旧名单未减。
三平台 SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 项及各 15 次真实 HTTP
均过；实际 SDK DLL 与 CLI 内核两路各七案及标记齐。ASan 174 来源、1913 CASE、
79290 断言过；749 份测试源码、私有附件及四份配置输入与 Git 原件吻合，清单
不覆盖全部生产源码。77 册产物、1940 份原件与封存副本逐大小和 SHA256 核过。
TSan 本片跳过，不计通过。未跑本地 CI。同步共享底座已交，自动 Memory owner、
候选管理、写回与完整持久身份及来源仍欠账。
[Memory 抽取线程启动失败](sdk-memory-worker-start-cleanup.md) 已随
[#340](https://github.com/relic-yuexi/LubanCode/pull/340) 合入 `9da20137`，与验收源
`e3d725fc` 同树。Windows/Linux/macOS 全 CLI 各 799/797/800 项均过，旧名单未减。
三平台 SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 项及各 15 次真实 HTTP
均过；新增五案与五标记在三平台全量及 ASan 齐。ASan 175 来源、1918 CASE、
79663 断言过；750 份测试源码、私有附件与配置输入已核，名单不覆盖全部生产源码。
77 册产物、1940 份原件与封存副本逐大小和 SHA256 核过。TSan 本片跳过，不计通过。
未跑本地 CI。后台异常、可撤销旁路借用、持久回合身份与用量来源仍欠账。
[Memory 后台异常收场](sdk-memory-worker-exception-cleanup.md) 已随
[#341](https://github.com/relic-yuexi/LubanCode/pull/341) 合入 `16d02a22`，与验收源
`db592938` 同树。Windows/Linux/macOS 全 CLI 各 800/798/801 项均过，各只添本片
异常 source，旧名单未减。SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 案及
各 15 次真实 HTTP 均过；新增七案七标记在三平台全量与 ASan 均实际执行。
ASan 176 来源、1925 CASE、79929 断言过；751 份测试及 profile 输入与 Git 原件
相符，这份名单不指生产依赖闭包。74 份产物、1925 份原件逐大小
和 SHA256 核过。TSan 本片跳过，不计通过。未跑本地 CI。
[Memory 持久触发身份](sdk-memory-durable-trigger-identity.md) 已随
[#342](https://github.com/relic-yuexi/LubanCode/pull/342) 合入 `77b8817f`，与验收源
`1b912d47` 同树。Windows/Linux/macOS 全 CLI 各 801/799/802 项均过，各只添本片
身份 source，旧名单未减。新增七案七标记在三平台与 ASan 均实际执行。
SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 案及各 15 次真实 HTTP 均过。
ASan 177 来源、1932 CASE、81050 断言过；
752 份测试及 profile 输入核对 Git 原件，不指生产依赖闭包。
74 份产物、1925 份原件逐大小和 SHA256 核过。
TSan 本片跳过，不计通过。未跑本地 CI。
[旁路短借 owner](sdk-memory-bypass-lease-owner.md) 已随 #343 合入 `bf465cb7`，与验收源 `f5cfdbd0` 同树。
Windows/Linux/macOS 全 CLI 各 802/800/803 项均过，旧名单保留；十六案十六标记在三平台和 ASan 齐。
ASan 178 来源、1948 CASE、89295 断言过；753 份测试及 profile 输入已核 Git 原件。
SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 案及各 15 次真实 HTTP 均过。
TSan 十五条注册均运行：十三份 Workflow 有案，一份已退役为空，另有 owner 十六案十六标记；十三条实际编译路径已核线程插桩，空来源不算有效测试。75 份产物、1938 份原件逐大小和 SHA256 已核。未跑本地 CI。
[标题精炼启动失败清理](sdk-title-start-cleanup.md) 已随 #344 合入 `163d793b`，与验收源 `83bfede6` 同树。
Windows/Linux/macOS 全 CLI 各 803/801/804 项均过，旧名单保留；标题七案七标记在三平台和 ASan 齐。
ASan 179 来源、1955 CASE、87224 断言过；754 份测试及 profile 输入已核 Git 原件。
SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 案及各 15 次真实 HTTP 均过。
TSan 保住十三条实际插桩路径与 owner 十六案；本片标题七案不在 TSan 来源，不称线程插桩已验。
78 份产物、1947 份原件逐大小和 SHA256 已核。未跑本地 CI。
旧源 `4497cfc7` 的 Windows Memory 批量提交失败原件保留；`83bfede6` 只补阶段诊断，不宣称生产读写竞争已修复。
[标题后台函数体异常](sdk-title-worker-execution.md) 已随 #345 合入 `e55bba48`，与验收源 `4950ff79` 同树。
Windows/Linux/macOS 全 CLI 各 803/801/804 项通过，名单与 #344 相同；标题十四案、两组七标记在三平台与 ASan 齐。
ASan 179 来源、1962 CASE、84740 断言通过；754 份测试及 profile 输入已核 Git 原件。
SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 案及各 15 次真实 HTTP 均过。
TSan 十三条实际插桩编译路径与 owner 十六案照留；标题十四案不在 TSan 注册来源，不称已由 TSan 执行。
78 份产物、1947 份原件逐大小与 SHA256 已核。未跑本地 CI。
结果发布、线程退出与闭包析构各有边界；七秒只圈取消后结果等待。
[SDK Backend 回调与最终析构](sdk-backend-callback-owner.md) 已随 #346 合入 `784fd543`，与验收源 `695ea229` 同树。
Windows/Linux/macOS 全 CLI 各 803/801/804 项，SDK ON/OFF 各 73/71 项，九套消费各 37/34 案与各 15 次真实 HTTP 均过，旧名单未减。
生命周期十九案与七枚 owner 标记在十三处规范原件齐；后台回调、最后析构、控制块分配失败和观察口异常回滚都核过。
ASan 179 来源、1969 CASE、89190 断言过；754 份测试与 profile 输入已核，不指完整生产源码闭包。
普通 SDK 不带私有头与三个 owner 测试导出。TSan 本片按路径跳过，不计通过，不借前驱绿灯。
74 份产物、1925 份原件已逐大小与 SHA256 封存；未跑本地 CI。
[Memory 共用词法门控](sdk-memory-learning-gates.md) 已随 [#347](https://github.com/relic-yuexi/LubanCode/pull/347) 合入 `33a057865224`，与验收源 `e6e931ad` 同树。
Windows/Linux/macOS 全 CLI 803/801/804 项均过；SDK Lua ON/OFF 73/71 案及九套移位消费均已核同源原件。
SDK DLL 与 CLI 内核两路抽取各十案十标记，Backend 十九案七标记保住；三平台专门 Memory 门及全量 Memory 案均过。
ASan 179 来源、1975 CASE；77 份产物、1940 份原件已逐大小与 SHA256 封存。
TSan 本片跳过，不计通过；未跑本地 CI。
[SDK 用量快照](sdk-memory-usage-snapshot.md) 已随 [#348](https://github.com/relic-yuexi/LubanCode/pull/348) 合入 `c613f195`，与验收源 `35468c7f` 同树。
Windows/Linux/macOS 全量 803/801/804、SDK ON/OFF 73/71 与九套安装消费原件齐；旧名单未减。
ASan 180 来源、1994 CASE、89056 断言；74 份产物、1925 份原件已封存。TSan 与本片专门 Memory 门跳过，不计通过；完整 CLI 另核 Memory 提交十二案、项目 Memory 四十七案。未跑本地 CI。
下一片[五项用量与来源](sdk-memory-five-field-provenance.md) 先交合同，完整实现与验收尚未交付。
合同已提交为 `3ba72dc7`。候选实现已迁入工作树；旁路采样传逐字段来源，汇总检查溢出并记覆盖。
主回合改在每趟请求返回处交用量事件，成功不再发第二回；ID-only 事件不清用量、不冒充明报零。
事件材料先验界限再序列化；已补实际采样、主回合取消、坏正文、重试和 assembler 身份用例，尚未运行。
公开 Operation 已接五项汇总与覆盖，主请求、子场转发分账；实际 Session 的关闭、同 ID 恢复、损坏记录拒收与旧记录缺省用例已写，尚未运行。
材料解码先验数量、字节和索引，再复制；CLI 报告也接回来源。超界字面量的解析前捕获、完整分配及回调失败、逐请求原材料持久恢复仍欠。
旧 Backend 已补输入、输出来源，明确零与负数照记；新增三项无材料仍未知，数字快照与来源快照不重复累加。
Chat、Responses 的坏明细容器已留原材料并阻止别名回退；原正文用例保留断言，新增事实帧另验，旧 CASE 名单未减。新增用例尚未运行。
错型别名只剩有界摘要时，现记“无法判同”，不把同长度或同类型认作相等；编码、解码保留这笔异常。
已纠正 Chat 候选实现把来源挂错块终帧的问题，来源改随消息终帧交出；文本与工具两条实际 parser/assembler 收尾用例已写，尚未运行。
四家 parser 已记明确错误状态；晚到用量仍交事实帧，正文和成功终帧不再越过错误。四条实际 parser/assembler 错误后收账用例已写，尚未运行。
Chat、Gemini、Responses 已留首枚与最新冲突响应号的有界见证；冲突作用于五项字段，汇总保住数字并关闭精确判定。仅换号也能给已有快照补异常，无用量不造快照。
跨字段异常用材料 JSON v2；旧 v1 读法拒收，新读法仍认旧材料。范围数量、重复字段和见证先验界限。三家实际 parser 的换号、晚到用量、无用量及版本栅栏用例已写，尚未运行。
本片未发布 PR、未交三平台 CI，不能计作交付。
异常归因已窄修：Generate 自身抛错仍归 API；拿到回复后，来源或观察回调抛错才按取消旗裁决。标准异常、分配异常、未知异常各有实际公开 Backend 用例；两次快照消费后抛错须保住五项数字、不吐正文或成功终帧。用例尚未运行。
assembler 先收终帧数字，再整理正文，最后记成功原因。新快照复制材料前先撤旧材料；复制失败不借旧来源证明新数字。分配失败全链路仍未验收，不能拿这处改序充作完成。
四家流式 parser 已接解析前有界数字扫描：保原大整数和浮点字面量；DOM 溢出或后续正文坏语法时，已识别数字照交，另记五字段 ParseIncomplete，拒绝成功终帧。重复已知字段或容器撤旧数字；正文同名字段不入账。四家实际 parser 用例已写，尚未运行。非流式入口、完整身份解码与材料分配失败仍须收齐，不能宣称全链路已交。
Responses 非流式回退已接原字面量和解析失败收账，成功终帧沿用同份材料。坏帧身份扫描补 Unicode 转义、代理对和解码后字节帽；错型、空字符、坏代理对、超帽及末枚错型身份不借旧号。实际回退入口用例已写，尚未运行。所有四家 Connection、分配及容量失败和逐请求材料持久恢复仍欠验收。
解析未完标记不再挤掉旧异常。异常数量或材料字节超帽时，内部快照留数字和原材料，公开快照撤掉未准入来源，parser 报错收口。满册、余一格、重复标记与字节帽用例已写，尚未运行；这处容量处理不替代全链路分配失败验收。
SDK 主请求已接借用用量入口：backend 返回后，先写 Operation 五项汇总与覆盖，再刷显示尾巴、复制 UsageReport 和构造事件 JSON。内存回执阻止事件重复收费，子场转发撤回执，父场独立收账；回执不进线协议。真实适配器的后续观察异常与子场转发用例已写，尚未运行。transport 中途抛错、子场序列化前收账和逐请求材料恢复仍欠，不能宣称分配失败全链路收齐。
主尝试已加 transport 异常退场守卫：有明报、真实身份或旧非零数字才收账；未见事实不造账。正常返回先退守卫，后续失败不重复收费。未完成尝试关闭五项精确判定，已带异常的字段不重增次数；观察者抛错不替换原 transport 异常。实际 Agent 的标准、分配、未知异常及旧终帧路径，另加 SDK 覆盖计数恢复用例，均已写、尚未运行。子场提前收账、完整材料恢复和全链路分配失败仍欠。
前台子场已在台账与事件分配前借用父场收账口；嵌套前台适配器沿同口上交，既有阻塞调用和 ExecutionTurnScope 收尽借用后退场。已收回执显式带过转发，陌生回执缺省仍撤掉。SDK 汇总、冻结和重置共用短锁，不在锁内调模型、持久写入、事件回调或 join。安装消费者原成功与父授权场景添五项分账、覆盖守恒及同 ID 恢复零重放验收，尚未运行。并行实际证据、原材料持久恢复及全链路分配失败仍欠。
逐请求来源现以借用上下文送到收账 owner：prepared 请求号、真实响应号、请求模型、step、turn、用途和发送前冻结缓存轮次分别留值；未接线身份留空，不造替身。异常退出和前台子场沿同一上下文转交；重试用例添两次实际响应号、用途与模型检查，尚未运行。报告另留规范用途，owner 尚未把逐请求原材料持久化，本轮只补真实来源接线。
sent 后物理交接、完整来源、公开自动 Memory owner 与 CLI 装配仍欠账。
CLI 主入口与 one-shot 尚未迁完，SDK 与 CLI 全面对齐仍欠账。下文保留历史原账。

核查日期：2026-10-06。已合基线为功能分支 `fd76b6a5`；[#333](https://github.com/relic-yuexi/LubanCode/pull/333) 的已验源 `c522a51c` 与合入头同树。

目标：新宿主只调用公开 SDK，便能运行完整会话，不必复制内部运行栈。CLI 逐项迁入，原功能保住。现已能嵌入；CLI 主入口和 one-shot 仍用内部装配，尚未全部对齐。

## 2026-10-06 批次记录：收口后再推进

当前只留总 Draft [#234](https://github.com/relic-yuexi/LubanCode/pull/234)。
功能分支停在已验 `fd76b6a5`。候选沿原 CI 分支收齐，不添实施 PR。

修复源 `a23f98e6` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37452164239)
已自然结束，整轮失败。原生全量 Linux 787、Windows 789、macOS 790 项均过；
九套 SDK、ASan 与 TSan 也过。Windows 三道后置纯数据检查读取 UTF-8 YAML 时
误用 cp1252，统一在字节 27 报解码错误。三份 extractor 已生成通过原件，不能
据此抹掉后续失败。首失败 19 件／5617043 字节已逐件复核、封存；摘要
`c0db4433643b849fe8ee4d7a335b09574b06650c081df20fbb4099d4250130ab`。

下一候选合入[显式 UTF-8 读取](ci-evidence-utf8.md)：只改三份纯测试的九处文本
读取，原字节、失败门、CASE 与预算保留。原十五项纯测试及 cp1252 默认读取
重放均过；C++ 新组合仍待本源远端 CI。未合功能分支，未添 PR。

上一源 `e79eea52` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37439210845)
已自然结束，整轮失败。1048 件原件逐大小与 SHA256 复核后封账；共
20707282 字节，摘要为
`c35878f0b1a59a536e19b7a1d8b426cb420e14776706bc606dea7b38cf1aba54`。
六套安装消费各 37 项中 36 过。不可变结果写入已报未知，后续新操作仍成功，
原验收拦住这条漏口；focused、OFF 与全量随后跳过。ASan 实跑 168 册，
167 过，同一公开结果案失败。TSan 实跑 14 册，13 过；workflow 五案开场
均报 `named_result.plan_path_rejected`，保留实际路径，未采宿主别名指向。
不能断唯一起因，也不能把开场失败写成 race。Windows 两套子场与恢复种子
当次实际通过；局部通过不算整批验收。

本候选保留 [SDK 入参测算](sdk-model-input-projection.md)、
[File 预览路径兼容](sdk-file-result-display-path.md)、
[原截止断言分组](sdk-owned-deadline-doctest.md)，以及失败时才输出的
[子会话终态](sdk-child-terminal-diagnostic.md)和
[workflow 开场诊断](workflow-session-open-diagnostic.md)。
测算共用实际 `Generate` 转换；SDK 输入、provider wire、请求输出帽分别记范围。
主请求只留指纹，摘要保真实材料。旧预算、断言和公开后端 ABI 照留。

这轮收三处窄修：

- [未知发布执行门](sdk-named-publication-fence.md)同时守新输入、已排队输入和后台命令启动。
  晚 Job 报未知，仍保父场已确认终态；同轮排队命令不得越过许可再启动。
  这条门只管当前 owner，尚未交付跨进程未知发布恢复。
- [Journal 材料门](sdk-journal-owner-operation-material-gate.md)改核实际操作账、输入与结果文件。
  原独立读回错用了 Jobs 专属绑定事件。旧档缺材料仍拒；新 CI 须留下实际生产文件。
- [File 路径门](sdk-named-owned-paths.md)收在自持会话根内，根和场内链接继续拒。
  宿主祖先沿既有 LocalTrusted 合同。Windows 原路径先留住 `..` 再查边界；
  真正相对路径验收在当前测试目录另建独立目录，不要求 Temp 与测试目录同盘。

路径补六场原生验收源码，核真实 SDK 会话、同 ID 恢复、File 材料与政策、
关闭后读取，以及根、内部目录、叶和悬空链接。Windows 造真实目录 junction；
POSIX 造文件和目录 symlink，不能据此声称 Windows 文件 symlink 也已验。
原 Named 10／8、workflow 五案、所有原预算和案例保住。

`a23f98e6` 实际六套 Lua ON 各 69 册 focused／552 CASE／37 项安装消费；
三套 OFF 各 67 册／537 CASE／34 项消费。ASan 实跑 169 册，76 册重点来源
与 26 份编译支持件已核；1869 CASE／75185 断言通过。TSan 十四册通过。
这些事实只属该失败源，下一份 C++ 组合仍须重跑三平台与消毒器。

下一片已在同一私有候选合并，源码交叉审查通过，尚待本源远端验收：
[十处场内 live 读取](sdk-journal-live-read-capture.md)取真正 Writer 捕获；
[开场五模块](sdk-prepare-journal-capture.md)共用本次惰性 File 捕获与同一只读视图。
模块首错与原装配顺序照留。小份来源指纹随原恢复请求入锁，锁后另取真实材料，
先核原引用，再比指纹，末后交原恢复适配器与续写接口；指纹不代替原生锚和 EOF 检查。
有效却归属别场的主账保原模块首错；真实账在两次捕获间变动，则拒绝续写。
原私有模块调用未带共享 owner 时，仍用原 File 读取。

[Policy 回调退场](sdk-policy-callback-lifecycle.md)复用 SDK 原阻塞门。
提供者调用、通知、退订和最后一份捕获销毁均入门；异步线程退捕获也照办。
通知只负责失效，原跨订阅与外部排空次序照留。公开声明未扩，只补四行寿命说明。
阻塞调用报重入并不会延后对象析构；宿主须留住仍在使用的场、Runtime 和事件流。

[CLI 单次提问 owner](cli-oneshot-execution-owner.md)已接入这批私有组合。
AskOnce 借用原 Spinner 与 ToolRuntime，Agent 改由共用 ExecutionOwner 创建和退场。
它没有把借用资源冒充自持 SessionResources，也没有将 CLI 后端折成文本 SDK DTO。
原请求、权限、工具、RunTurn、受理与关账照留；源码独审通过，原生尚待远端验收。
完整 JournalStore、托管授权和 CLI 主入口迁移仍未完成。完整 AskOnce 端到端
证据须另补，当前 owner 案不能代替整条命令验收。

[交互 CLI owner](cli-interactive-execution-owner.md)也已收入私有组合。独审拦下
首版重建后命令表仍借旧 Agent 指针的漏口；[稳定槽修复](cli-stable-execution-owner.md)
已过独审。owner 只创建一次，后续在原槽重建；旧命令借用成功重建后仍同址。
复制或恢复失败则留空，宿主仍须守原串行与空槽纪律。原七 CASE 全部保留，
新增实际旧指针、引用和闭包调用验收；这份新源码尚待远端执行。

[逐次模型发送门](sdk-model-send-gate.md)位于真实 AgentLoop，每次首发、恢复重试
和后续模型步都在提交 sent 预算前检查。拒绝、异常和取消保原账，机密失败串不外泄。
它尚未绑定 Policy 或原发起者；摘要、Compact、采样与后端内部网络重试仍不受此门管。

[Managed 新场底层准入](sdk-managed-session-admission.md)把真正锁和原持久回执
整束交给 Manager，首条 V3 写完整创建身份。失败和 Close 后均禁 Local 回落；
候选先收 Writer 和附属写帽，再放锁，非空残账继续保留。现有 Local 场拒隐式转换。
这笔只完成内部存储链；公开 Managed SDK、执行授权、同 ID 恢复与旧账迁入仍未交付。

## 八阶段现状

| 阶段 | 已合范围 | 接下来补什么 |
| --- | --- | --- |
| 收口当前批 | Worker #245、Runner #246、SDK #325、Job 日志 #332、Web／可选 Lua 等组合 #333 已合功能分支；旧子 PR 已收口 | Jobs／不可变结果组合在已有 CI 分支验收；当前只留总 Draft #234 |
| SDK 完整化 | 五件原内置工具、有界 web_fetch 和 Todo；显式 Skills；项目 Memory Recall/Save 和片段 CAS；前台深度一子 Agent；主 Action；strict standalone Lua；Package 清单分析与根 inventory；公开 RAG 参考例 | 其余 CLI 工具、自动 Memory、Skills 管理、Package 挂载、后台与嵌套子任务、公开 Job、CLI 主入口迁移 |
| 四口 SPI | Session 真接 EventSink；召回片段真接 Memory Blob provider；公开 PolicyProvider 原语 | 完整 JournalStore/BlobStore，替换结果与恢复读写，接真实 Session 授权；本阶段不引数据库 |
| 依赖瘦身 | updater、Release 查询、渠道、Gateway 留宿主；Package 只带中立 parser；Lua 可从 SDK 依赖闭包关闭，ON/OFF 三平台均已验收并合入 | 继续检查依赖闭包与端侧最小装配 |
| 身份与治理 | 身份值、授权 action、可撤销 PolicyProvider 和订阅合同 | 新托管会话 ownership、执行前重查、恢复归属、查询与审批隔离；旧账迁入另批 |
| 公开服务 | AppServer 内部网页、WebSocket；本地可信 Worker IPC | 正式 Worker 协议、HTTP/SSE、持久 outbox/ACK 和游标；gRPC 按需 |
| 分布式与存储 | 本地 Worker；独立实验 Runner；两端部署合同 | 网络登记、心跳、鉴权、存储与队列适配；池化和 Sandbox 池各守前置门 |
| 扩展与编排 | 可信 C++ 窄中间件；前台子 Agent 与主 Action | 动态库、热卸载、沙箱、工作流与多 Agent 的公开装配 |

上述范围只算已经合入功能分支。接口存在不等于真实执行已接管，私有候选通过也不等于交付。GPU 占用和网络连通交用户处理；同项目可开多场，不能把 Session 当成独占工作目录。

## 已合基线

[#325 原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357534) 和[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357428) 均过。六套 SDK 各有 28 场安装移位消费、48 册 focused、414 条原生 CASE；全量 Linux 744、Windows 746、macOS 747；ASan 实跑 147 册，54 册重点来源通过。

859 件材料逐大小与 SHA256 复核，封账摘要为 `e82c9e15e01e0150287bb03ebb1c10f2326a6038a323fd3f5e450b7c98822278`。全量结论取实际日志与保留原件；并未上传每册完整 JUnit/LastTest。LSan 关闭，浏览器和 TSan 按路径跳过。SDK 插桩不能替 Worker/Runner 插桩。

## 已收口：#332

[#332](https://github.com/relic-yuexi/LubanCode/pull/332) 源 `0066ac3e` 已合进功能分支 `6e2702b2`。它补[真实 Job 来源绑定](sdk-job-operation-binding-v1.md)与 [V3 日志回执](v3-journal-witness.md)，并同步 main `3b973ffe`。Job 历史只读 PassiveHold；首次未确认追加和首次 checked Close 持值，不靠重复 Close 改口。这批仍属内部前置，尚未开放后台 Job 或完整 JournalStore。

本源[原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915212)和[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915234)整轮均过。六套 SDK 各 50 册 focused／426 CASE／28 场安装移位消费；全量 Linux 748、Windows 750、macOS 751；ASan 实跑 149 册、56 册重点门、1742 CASE／65795 断言。559 件材料逐件复核，封账摘要 `b4618090fd1026b3c538de9a4dbb5995f7402e4a202ec4525d9865c7778a473c`。scoped PowerShell 包装器三枚 cmdlet 改用模块限定名，原管道、格式、错误流和退出码保住；原第六案末补 legacy/scoped 对象表格逐字对照。Windows 两套 focused exact 实耗 2242／2299ms，两份实际对象表格均 76 字节且逐字相等；全量双源码也保登记、JUnit 与 LastTest。审查、评论与线程无待办，合入前重查源头和目标未漂移。

前源 `52fa7d6d` 的[原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37250651084)整轮失败；[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37250651072)通过。它留下这些实际结果：

- 三平台全量 Linux 748、Windows 750、macOS 751 与 ASan 149/56 均过。新 Job/V3 两册各六案保实际 argv、非零断言与原始日志。
- 五套 SDK focused 通过；Windows SDK-only 50 册中 49 过、一册失败。六套安装移位消费各 28 项通过。
- 失败册 `sdk.focused.run_command_execution_limits` 六案五过、一案失败。`powershell-exact` 实际登记 `RUN_SERIAL=true`，仍等到 15054ms 超时。同次 shell-entry、wrapper-ready 确认；user-block-entry 未确认，probe 起步和结束记录均缺。原帽仍为 15000ms，未加时限、预热或重跑求绿。
- 监督器五案通过。健康拍夹具只由手动拍驱动，消除背景拍先改状态、稍后入通知时断言抢跑；生产监督器未改。
- 失败原件已封 568 件，摘要 `96ce13da2b5e4d8131638ec2dc9372f7c3a90064ac1f6ea6b2a96462773fec60`。Windows 全量另保完整实际登记、JUnit 和 LastTest；ASan 保 raw File API、149 册实跑与 56 册重点门。

入口观测只收窄待查区间，尚不能断唯一起因。全量较后通过不能盖掉这次失败。新源修复已单独验收，失败原件继续封存。

## 已收口：#333

这批保留 #332 的 `0066ac3e` 窄修复，接入
[有界 web_fetch](sdk-web-fetch.md)。宿主显式选择工具并冻结每场上限；默认不开放。
公开配置与能力快照只含 SDK／标准库值，内部单次请求 Transport 留替换口。
模型只能收窄预算，Close 等请求真退出；不支持 gzip 时明确拒绝编码，不添解压依赖。
原 CLI 19 案保住，另补两案；SDK 六案和安装消费核实际 HTTP、预算、取消与四场隔离，
HTTP 夹具另核线程退净。公开 Todo、Package 根 inventory、可关闭 Lua 与安装后的 RAG 参考例同批收口。

源 `c522a51c` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37309659950) 15 项全过，[#333](https://github.com/relic-yuexi/LubanCode/pull/333) 已正常合成 `fd76b6a5`，合入树精确相同。六套 Lua ON 各 59 来源／486 CASE／34 场安装消费；三套 OFF 各 57 来源／471 CASE／31 场消费。全量 Linux 766、Windows 768、macOS 769；ASan 实跑 158 来源、65 重点门、1802 CASE／69955 断言；TSan 十四场 workflow。九套 Web、RAG 与原始图均核过实际执行，三份跨镜像包各跑五笔命令。1061 件原件逐大小、来源与 SHA256 复核，摘要 `c0ac5d8b6cde2c1c8441e13629aed29f2bf77733ac253c2ede6d52227d851b53`。LSan 关闭；Linux/macOS 全量以真实 job 完成日志为据，没有逐来源完整 JUnit。这批绿灯不借给新 Jobs／不可变结果／SPI。

首组合 `92f93572` 的[远程 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37284807084)在重复 Location 安装消费报普通网络错，macOS 33／34 通过；HTTP 第 21 笔后停止，服务正常退净。原日志未记底层 CPR 错误码。查 curl 源码，部分版本会在第二枚头进入回调前拒绝协议；这条差异能解释现象，不能据此断定唯一成因。

窄修只保底层实际失败状态，并将 `NetworkFailed`、CPR `WEIRD_SERVER_REPLY` 和真实跳转状态同时成立时归为 `redirect_invalid`。没有状态、304、普通状态和其它网络码不套；取消、时限与字节帽仍先判。不解析错误文案，不拿残留响应跟随跳转。原案、HTTP 夹具和预算照留，23 项纯门、文档与独立源码复核通过。新组合 `57d70c91` 已同步功能分支，[本源 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37288325660)整轮失败：Windows combined 四场生命周期案撞 Memory 活锁；Web 九套原生与安装消费均核过实际 30 笔 HTTP 和正常退场。

本源预期 ON 六套各 55 册 focused／458 CASE／34 场消费，OFF 三套各
53 册／443 CASE／31 场消费；全量 Linux 758、Windows 760、macOS 761；
ASan 154 册实跑、61 册重点来源及 19 份支持件。预期不能代替实际原件。

私有源 `37219c98` 的[整轮 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37245678268) 已通过，仍未合入功能分支。

| 模块 | 合同与已验范围 |
| --- | --- |
| [Package 盘点](sdk-package-inventory.md) | 显式根、有界目录与文件清单；不挂载、不执行、不替宿主授信 |
| Lua ON/OFF | 默认 ON；OFF 真去编译依赖；三平台跨画像恢复，在读取所选 Lua 脚本、创建 VM 和调用模型前拒绝启用的 Lua 来源 |
| [主场 Todo](sdk-todo-write.md) | 每场自持清单，整表替换；恢复保历史、重新起空表；CLI 提醒与展示留宿主 |
| [Agentic RAG 参考例](sdk-agentic-rag-example.md) | 仅公开 SDK 和标准库；宿主注入检索工具，真实模型工具回环；不增核心检索 API 或向量库 |

ON 六套各 54 册 focused、452 条原生 CASE、33 项安装消费；OFF 三套各 52 册、437 条 CASE、30 项消费。三平台跨画像各五条真实命令；九套独立 RAG 示例从四份复制源码单独构建和执行。全量 Linux 756、Windows 758、macOS 759；ASan 实跑 153 册，60 册重点来源通过，另核 18 份附件和实际 PCH。

876 件材料已封，摘要 `d1ec767ac5539b1b0c58fe6081f44db582fa29ac99f91a9bf89c03d109ab0264`。本地旧组合 `640c0f71` 只另加监督器夹具修复。上面的新组合还接了 scoped PowerShell 修复、Web 工具和验收门；不得借 `37219c98` 绿灯验收。

操作日志退场、ResultPolicy 开场锁与 Managed ownership 另留私有前置；真实 Managed 授权尚未接管。[后台 Job 合同](sdk-background-jobs.md)先定取消、许可和 Close 次序，尚未公开执行。

本地下一组合已收入[Owned 登记截止](sdk-owned-job-deadline.md)：正预算从真实登记追加起算，排队、采用和授权复核不重开时长；晚入线程保 Started 意图，实记命令未调用，不编业务 raw 或 Post。到期观察携真实父交付五引用；Hold 能核回未派发便取消的已确认交付，伪引用、错序和矛盾标记拒绝。旧零预算与 Legacy 函数保原行为，恢复仍只读，不重建执行截止。

这笔只在实际调用前收窄原有相对 process timeout；平台启动与 kill／capture／join 照原语义，不承诺截止那刻已退净。六条真实原生路径核登记／排队、授权耗时、运行预算、晚线程、启动抛错／Close，以及严格恢复。CI 另核完整 argv、六案、非零断言和 owned 终态事实；没有 command 就不能编 timeout，已调用就须有正预算，配额与线程必须真退净。三平台 SDK ON 预期各 56 册／464 CASE，OFF 各 54／449；安装消费仍为 34／31；全量预期 760／762／763，ASan 155 册／62 重点来源／19 支持件。源码与纯门已查，这份组合尚未完成原生验收。公共 SDK Job 仍未开启。

`6900159b` [首轮远端](https://github.com/relic-yuexi/LubanCode/actions/runs/37291725260)已失败，暴露两项接线漏处：Lua 画像纯册仍写旧名册 55，实际已增至 56；截止案两行 `REQUIRE_MESSAGE` 消息含裸条件式，编译报错。下一组合更新名册、只给条件式添括号；六案、预算及业务行为不改。三套 combined 和 ASan 均未执行 focused 或全量原生案，不能记作截止逻辑已通过；三套 combined 安装消费各 34 项及独立 RAG 示例实际通过。

下一组合还接[Managed 开场预留](managed-opening-reservation.md)与[归属侧记](managed-session-ownership.md)。内部 owner 先原子新建空目录、拿真实 SessionLock、缓存本次首笔耐久发布，再逐字重核归属，最后才建正文子目录。未知发布与失败残留留原样，不借重试补成已确认。LocalTrusted 开场、候选、祖先、管理与删除路径拒绝有标记或归属不明的账。原始 SDK 预读与完整 Policy 尚未接管，不能称公开 Managed 已交付。两册分别六案、十案，真实锁、未知发布、旧本地恢复、晚标记与祖先拒绝均须远端验收。

同批还接[Memory 项目交接](memory-project-commit-handoff.md)。`57d70c91` Windows 四场原件显示同项目第二场归属正确，却撞活锁而未写；原件没记锁释放，不能唯一认定慢盘或残锁。补同一实现场内的逐项目可取消队列，整段写入仍须拿原 OwnerLock，先放磁盘锁再交棒。异项目各自前进；坏锁照拒；等待取消、回调异常和同线程重入均不得悬票。原 20×100ms 外部重试、OwnerLock 与旧 24 案不变，另加六案核真实写入与线程退净。

这份开场／交接组合预期 ON 各 59 focused／486 CASE，OFF 各 57／471；消费仍 34／31。两册 Managed 与一册交接各注册 original 和 focused，全量较 `6900159b` 增六项，预期 Linux 766、Windows 768、macOS 769；ASan 158 册／65 重点门／19 支持件。这里只列验收名册，实际登记、执行、关场与完整 CI 仍须新源给证据。

`532ed159` 随后在启动前失败。GitHub 报分类步骤“表达式超过 21000 字符”，作业、附件与原生执行均为零。新修把 PR base、push before 两枚提交参数搬至 step 环境变量；分类脚本只读变量，整段不再含 GitHub 插值。路径、选择器、预算与原生案数照旧，另加纯门拦住把模板插值搬回长脚本。须等修正源真正启动并完成三平台验收。

修正源 `a7165eec` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37297587597)已失败。三套 SDK-only 又停在 Todo 纯门：这册仍写 ON 55／OFF 53，实际名册已到 59／57；RAG 同类旧数也须改齐。三套 combined、ASan 与 TSan 均在开场预留两行消息报编译错，字符串拼接也须整段加括号；focused／全量／ASan／TSan 原生均未执行。三平台安装消费各 34 项与独立 RAG 示例实际通过，不能替 focused 记过关。下一源只改两处名册数和两行消息括号，消费、CASE、生产逻辑与预算不变；整套 SDK 纯门本地 282 项通过，没有配置、编译或执行原生程序。

`80c8cd5a` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37299684343)已越过三套 SDK-only 数据门。macOS 编出 SDK 测试程序后，编译来源边界门拒绝三册新 shared 测试：CMake 已接入，独立白名单仍缺 Managed ownership、开场预留与 Memory 交接。`81ed65f2` 补这三条精确路径并[重跑远端](https://github.com/relic-yuexi/LubanCode/actions/runs/37302239817)；testing ON 且唯属 SDK reference target 才放行，testing OFF、其它 target 和近名新增来源照拒。边界、依赖闭包及开场／交接纯门 116 项通过。

同源 `80c8cd5a` Linux combined focused 59 源／486 CASE 实际通过；macOS 59 源中57源通过，Memory 交接与 SDK memory_save 两源失败。Managed 归属六案、开场预留十案和后台 deadline 六案在这两平台均实过。macOS 新项目的 memory 末段尚未建，旧 equivalent 错误分类令异项目两场及新交接两案报身份失败，归属未串场。下一源先核叶目录是否存在，再比 memory 或 workspace 身份，权限与未知错误仍拒；原24案、新六案、预算与磁盘锁不改。六份 ON 安装消费各34项和独立 RAG 示例已过，不能替失败 focused 记全绿；OFF、全量及消毒器继续按本源收原件。这处生产修正须另跑远端。

## 后续装配规矩

存储 SPI 前先补[结果仓不可变发布](result-immutable-publication.md)。File 默认实现用独占临时件和原生 no-replace 发布，正式名碰撞便拒，不能先查 exists 再 rename。逐枚保文件与直接父目录的实际确认；祖先链未确认、部分发布及未知回执照实留账。首次未知封 Store 后续写入，不读回升级；确定零发布的失败可继续。只交内部 File 前置，没有公开 JournalStore／BlobStore 或数据库。旧 AtomicWriteFile、预览／编号算法和原 CASE 保留，另添八案；三平台 focused／全量与 ASan 须在新组合实跑。

结果仓前置现与 Command Jobs 合成同一候选，复用已有 CI 分支验收，不添子 PR。新结果册同时注册 original 与 focused；边界名单只准精确来源，故障注入册共用平台资源锁。两条分类分支、两份 ASan 选择器、首轮内联名册及实际路径／argv／CASE门一并核对；旧结果仓资源锁仍在原分支，新锁不遭后一次赋值覆盖。源码与纯门通过，原生尚待这份组合实跑。

每项先写短合同：公开什么，谁持资源，取消怎样传，关场怎样等借用退出，恢复依据哪份事实。只交实际接通的一段，不先铺一排空接口。

工具、模型、存储、事件和策略各留窄 seam。宿主显式配置能力，Session 冻结本场配置；关闭等在途调用真退出，未知回执保未知。未来网络、数据库和插件实现接这些 seam，不能让核心依赖具体 UI 或基础设施。

默认只出 preview。Full 须 Node 许可和每场参数同时开启。子 Agent 默认关闭，宿主显式给预算，许可只管当前子场。

保持一张实施 PR，收完再开下一笔；功能分支到 main 继续走 Draft [#234](https://github.com/relic-yuexi/LubanCode/pull/234)。用户已授权自行收口，仍须逐源审查和远端三平台 CI。源码、Git、纯数据和文档可本地检查；配置、编译、CTest 与原生执行全交远端。

## Command Jobs 接线候选

公开源码封于 `2d582d03`，这批从 `c522a51c` 合入并补 CI 接线，尚未合功能分支，也没有本组合远端通过证据。默认不开 Job；显式预算与真正 run_command 声明接到同一 Session owner。已接原父调用五引用、真实 Job Operation binding、只管本 Job 的审批、唯一 host worker 完成泵、取消与 Close 真 join，以及同 ID、同 run 的 Hold 只读恢复。ReadJobPreview 是可信本地缓存，不能当出站投影。

Jobs 两册17 CASE 加结果仓八案，共增三册25 CASE。合树静态名册为 SDK Lua ON/OFF 62/60 册、511/496 CASE，安装消费35/32场；全量预期 Linux/Windows/macOS 772/774/775。ASan 当前选择式选出重点68册、执行来源161册、wildcard编译正文161份、附件23份；这里列接线范围，不冒充实跑。公开 Blob／Journal SPI 尚未接入。

CI 会留安装/移位消费者原 argv、显式真实 probe 的 File API 归属与字节指纹、公开十路径、私有七路径，以及四份写后未知 typed 原件。注册、JUnit、LastTest 三者须相符，失败也留原件。原 deadline 六 CASE 和 CLI 命令路径不改；本地仅跑纯数据、AST、脚本语法与文档检查。详见 [Command Jobs 合同](sdk-command-jobs.md)。

### 首轮组合实测与窄修

`605d200f` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37312694121)整轮失败，241 件原证冻结，摘要 `defb4ccbc2f70540f1301e21ab4b20f2ceb26184d443db0496e3e8baac9f037b`。六套安装消费停在私有 probe 准备：SDK-only 原本关闭测试，图内没有该 target；Linux/macOS combined 只编 SDK 库，没有 probe 可执行件；Windows combined 图带真实 ZERO_CHECK 再生依赖，遭旧“零依赖”门拒。消费、focused、全量与 OFF 本轮均未执行。

ASan 真跑了 161 册，其中 159 过、两册失败；1827 CASE 中两案败，71195 断言中 11 条败。新 Jobs 公开十案、私有七案和不可变结果八案均实过，四份写后未知回执逐字段保住；TSan 十四册实过。这些局部结果不能验收整树。Lua 恢复坏声明仍遭拒绝，但会话身份先过 Jobs 绑定门，旧夹具硬认 Lua 错误码；子会话捕获夹具堵着旧固定临时名，独占临时件改名后未再触发故障。后源只修[明确预检期待和真实 no-replace 拒绝点](sdk-lua-child-precheck-repair.md)，保原硬断言，不改生产顺序。

probe 后源改用独立、标准库私有夹具，复制原源码，在 CI 临时目录单独配置和编译，再从真实 File API 取实际可执行件并移位。产品测试开关、SDK 闭包和原 CMake 图照旧。配置或编译失败也保有界原 reply；成功后仍严核源码、目标与 artifact 归属。上传的原图先记 `not_evaluated`，不能拿“已保存”充作“已通过”。安装消费还留真实 helper 与 jobs 头文件原字节。

旧 Windows ready 首读失败另补[原件与清场诊断](workspace-racer-ready-evidence.md)：保存首次字节、真实句柄与 argv，失败前收同一只 helper。六 CASE、原前缀断言、锁状态和预算照留；没有重读求绿，也未断言根因。这些后源改动只过源码与纯门，仍须新一轮远端验证。

`a9d3f16e` 的[后源 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37320718673) 又抓到两处错误，不能算通过。四条 Linux/macOS 安装消费者实际各 35 项中 34 项过；四场隔离已关场，随后的缺会话恢复错报 `sdk.job.plan_invalid`，违背旧 `sdk.session.open_failed`。Windows 独立探针配置与编译均成功，原始图同时列指定 EXE 和同目录 PDB，旧门把总产物数当可执行件数而拒绝。两份首次失败原件分别冻结 69 件与 29 件；没有补造未生成的消费总验收。

新候选只补[缺失分类](sdk-command-job-missing-resume.md)与[真实产物选择](sdk-command-probe-artifacts.md)。缺 workspace 或明确缺 session 沿旧错误码拒开，已有坏材料仍严查；原 private 案追加八次真实 Runtime 准入反例，旧七 CASE、消费者原断言与预算保留。Windows 精确选指定 EXE，可附同目录同名 PDB，POSIX 保单无后缀产物；生产与上传后复核共用同一门，原图字节不动。新的原生结果仍须独立交远端 CI。

`7a5321c8` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37421453525)整轮失败。唯一失败在 Windows SDK-only ON：focused 62 源中 61 过；Owned deadline 六案五过，438 行想读 `queued`，实际已为 `cancelled`。原六份终态事实仍齐；Windows OFF 未执行。其它五套 ON 各 62 源／511 CASE、两套 OFF 各 60／496、八套安装消费 35／32、三平台全量 772／774／775、ASan 161 来源／1827 CASE／71878 断言及 TSan 十四场 workflow 均过，不能替失败通道验收。1257 件原件共 49246151 字节，逐件复核后冻结，摘要 `6f1a67089a5b601af8b890867ac8db5137d597c787ea30d6c346431be1ab7faa`。首次失败 15 件与原封条 `e48f85c030b8904c61850ec654d9f7a5773afc3b5709cf5de1e1988761c45b05` 继续保留。

后源按[排队观察合同](sdk-owned-queue-observation.md)保住原 2000ms 正预算和全部截止、零调用、父引用、恢复与退场检查。另加零注册预算真票，硬查堵在活进程后排队，再取消退场。正票快照若已取消，还须核 Register 前实际时钟已过最早截止；其它状态照拒。生产、原六 CASE 和预算不变。纯数据门已核，原生仍交新源远端 CI。

## NamedResults 接线候选

[named 结果合同](sdk-named-result-blobs.md)先于实现封存。源码 `1a92b2ca` 接通一个 Session 所有 named 结果入口：原始捕获、正式结果、Job 准入、Job 完成和列表材料共用 Store、编号与整份写 lease。宿主显式提供 Provider；默认仍用 File。结果投影、读面与同 ID 恢复沿真实句柄取材料，不造本地镜像。外部仓只认本场冻结 namespace，不能替 Session 授权，也不能兼任 Memory CAS 或 Journal。

写入首次未知便封后续写。确定尚未调用 Provider 的失败另记；原生 File 确认和宿主声明分开留值。Close 等在途执行和写借用退出，只读句柄仍可存活，不能拉住 Writer 或 SessionService。Runtime 开场离开注册锁，Shutdown 等所有已准入开场完成或退场；Provider 回调递归读和阻塞生命周期调用在拿锁前拒绝。

独立源码审查已核 37 条变更与 767 份旧原生测试锚，未查出阻塞项；File 旧流读、预览、编号与异常语义保留。公开十案用安装 SDK/STL 与真实第二根字节，私有八案再核原生回执、开场身份、未知持值和退场。自动摘要另保真实 V3 证据与后续模型请求确认；安装消费者没有内部主账读面，不能冒称同样检查。

这份接线合入 `5498f954` 排队观察修正，旧预算与终态断言保留。安装消费多一场 named-results，SDK focused 多两册、18 CASE；新名册须逐项核 actual argv、JUnit、LastTest、公开头／helper 原字节和编译归属。此处只记源码候选，尚未合功能分支；整套三平台和 ASan 要在新源远端 CI 实跑，旧绿灯不借给它。

集成源 `bb9fb3f1` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37429745834)整轮失败。八条原生构建均停在 `named_result_blobs.cpp:26`：`string_view` 不能隐式转给只收 `const string&` 的 UTF-8 检查。消费者、focused、全量、NamedResults、ASan 和 TSan 均未执行；ASan 名册 163／70／25 只是计划。219 件失败原件共 4713248 字节已封，摘要 `9076087630e461d7643b96e53fc4516ef8e268a11f76f371d8f45787401a9776`。

窄修 `68e1fcf2` 只加显式 `std::string(value)`，其余 3036 份文件未动，源码与实际接口逐字核过。[新源 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37431764520)复用原分支，旧轮结束并封账后正常快进。它已越过这处编译错误，整轮仍失败；实际范围见本页“当前批”。未添实施 PR，未在本地编译。

## Journal 所有权接线候选

[主场 Journal 合同](sdk-journal-owner.md)先于实现封存。当前先归拢真实 V3 File 账，保旧 JournalWriter 和 File anchor。AppendLease 独持共享状态，原生追加与内存更新共守写门；首次未知先封、再解锁、最后退引用。原生已提交而内存更新抛错，另记 semantic unknown，不改称尚未写入。读句柄只持捕获字节和原生文件锚，不能吊住 Session 或 Writer。

同 ID 恢复在真实 SessionLock 内只捕获一份材料，投影与 continuation 共用它，严核原对象、完整前缀与 EOF。旧 RecoveryView 字符串接口尚多留一份主账字节；原 128MiB 帽管逐份读取，没冒称总驻留上限。SDK 各项 Prepare/restore 尚未全归这处读面，公开 JournalProvider 要等这条线收齐。本阶段不引数据库。

源码 `9093af67` 已闭合独审抓出的两处夹具错误：doctest 消息三元式加括号；一次真实工具调用按 action/attempt 分组，恰核 raw capture 与 formal 两份，恢复和关场后逐原身份、元数据与字节比对。原七案内部验收、三场 SDK/STL 宿主验收、一场私有主账核验留着。独审只算源码通过，尚无远端原生结论。

这批正接编译归属、安装移位、ASan 与同次全量原件。宿主先开场、跑完整回环、Close，再换 Runtime 恢复同一 ID，核零重放，再跑一回；同项目另开一场核历史与执行不串。受控验收账、原 raw/formal 文件随失败也保存，保存不等于通过。公开 SDK 不添内部 Writer ABI，完整 canonical/hash/Prepared chain 仍交实际原生 guard 验证。
