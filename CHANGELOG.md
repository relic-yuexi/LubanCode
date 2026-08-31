# 更新记录

这里只记用户看得见的变化。每个版本留三条，细处可点版本标题查看提交差异。

## [v0.26.64] - 2026-08-27

- **模型与 provider 的切换收成一条路。** `/provider add` 存好即自动切到新家;`/model` 切到别家模型时连 provider 整套切换并明示;从列表选过的模型落进用户目录,中转家的模型不再"死目录判不认";切不动如实提示保旧连接。
- **Ctrl+V 直接贴图。** 剪贴板有图走直贴(与 Alt+V 同路),纯文本照旧粘贴零变化;目录向导里"自定义(全手填)"置顶,光标默认仍落第一家预设。
- **模型怪癖矩阵钉死 1300+ 家档。** minimal 从"当关"归位真档(GLM/gpt-5/gemini 轻量系用户选的最低思考档不再悄悄消失);Gemini 3 思考 token 摊账按服务端总数对账;新增任意 provider 一把梭的真端点抽验模板。

## [v0.26.66] - 2026-08-27

- **终端隔开数据与画面(第一步)。** SSE 流式回调不再直写终端,只投 UI 事件队列(洪峰合并、收口事件结构上丢不了);Markdown 解析挪出输出锁,心跳补画不再被饿死;改宽瞬间的屏幕账修齐——残影活动栏清扫、整屏重建不再丢正文、锚点漂出窗口自愈,宽度锤击拖长从三成到零。
- **后台子代理两条正路打通。** settings.local 预放行的工具后台照用(派出时刻定格的放行账,deny 名单仍压过);面板 x 停止真贯通到工具进程树,按下即见"停止中"回执,PowerShell 时代"ESC 杀不掉命令"的旧账一并修了。
- **模型协议兼容矩阵立账。** 思考方言写进目录(66 家,不再拿一只布尔猜各家 JSON);手册实录 fixture 11 册四种刀口重放,四家回环跑穿"思考→工具→第二轮→终答";探针按 manifest 跑哨兵,accepted/emitted/effective/accounted 四栏判定,没钥匙记 SKIP 不涂绿。忙时占位补回 Esc 打断提示,agent 工具缺参明说带示例并设连败保险。

## [v0.26.67] - 2026-08-27

- **输入框不再消失,忙时不再卡。** 重画失败降级画最小框或挂待补画旗,不再停在"框已擦没画回";footer 超高时按提示→坞→队列次序舍行,"输入行必画得下"成了布局硬约束;忙路改行级 diff 与指纹跳帧,每键不再全屏推倒重来。
- **一帧一次落屏。** UI 事件泵的消费线程按帧节拍收批,一帧只擦画一轮;千枚小 delta 与十枚六百字大 delta 两本压测账,输入框逐拍在位、心跳不饿死;终端不支持忙时重绘时打一行明示,键入照收。
- 压测顺带修掉平移视口报 0 也挪原点、帧整体错位的真 bug;Composer 闪失旧单(Enter 所有权、多行行尾光标)核账销案。

## [v0.26.68] - 2026-08-27

- **模型出图能接住了。** Responses 的 image_generation_call 全链接通:base64 验身限额解码、内容寻址原子落盘,终端给路径与尺寸,续聊只带引用不重放(真机三只图像模型各出一张 1254x1254,续聊输入 8.2k token);请求失败行带上 HTTP 状态与错误类型,不再只剩一句糊脸的"Upstream request failed"。
- **模型归属不再误报。** 从真机列表选出的模型先落痕再判定,首选零跨家提示;直输模型名只说"某家目录也收录,认不认以实测为准",不再断言中转站不认。
- **端点能力挂标。** Realtime 等非当前 wire 的模型在菜单挂 [Realtime] 标,确认前一行指路;目录可声明模型"只出图不吃推理",切换时停发推理字段、回来恢复。

## [v0.26.69] - 2026-08-27

- **思考时能瞥见内容了。** 思考流在默认画面暗色露出末尾三行,内容往前走画面跟着换;完毕立刻收成一行"思考 10.2s(Ctrl+O 展开)";运行中按 Ctrl+O 展开便随流续画,用户不关门程序不擅收。四家 wire(anthropic/chat/responses/gemini)的思考流喂同一组测试,画面一致;管道输出只留收定一行。
- **兼容端工具续轮收住。** vLLM 等兼容端的工具后思考变体形状入了 parser,MiniCPM5 真机实录 fixture 对账点亮;探针脚本另备,不进常规测试。
- 思考预览只在支持原地重绘的终端开启,其余明走降级,不画半只框。

## [v0.26.70] - 2026-08-28

- **打断是真快了,有据了。** 新刮屏驱动直写控制台输入队列重测:Esc 到断开回输入态 P50 26ms、P95 320ms,1 秒门内;此前 8.4 秒坐实为终端注键排队的假账。半截正文保留,下一轮照续。
- **思考关不掉时不再装聋。** 目录可声明某端点"未证实可关",/think none 切换与查看两处明说;/doctor effort 每档三回对照,HTTP 接受、思考产出、正文产出、终止原因四账分开,预算耗尽只判存疑,不拿 2xx 冒充行为支持。
- **纯文本模型发图前就拦。** 目录声明 image:false 的模型,带图输入整轮不发、当场给人话;未声明的照旧试探,服务端错误另有分型兜底。

## [v0.26.71] - 2026-08-28

- **GUI Agent 扩展示例来了。** 本地 process plugin 九件工具(离屏截图/坐标归一的移动点击/中文直注输入/键/滚轮/聚焦/枚举/状态),零第三方依赖,DPI 多屏全口径;配套 Skill 纪律(观察以最近为准、动作后必复验、盲点五次停手)、tkinter 夹具带事件账、离线自测 32 册。
- **真机实跑双绿。** 200% DPI 桌面上中文真进输入框、下拉键盘选值、提交真收到;挪窗后旧观察点击被 stale 拦截,不拿旧图坐标硬点。
- 示例代码即文档:排错表写明 DPI 口径、下拉浮层等真踩过的坑;"选 process plugin 还是 MCP"的取舍指南随 examples 总导航交付。

## [v0.26.72] - 2026-08-28

- **MCP 工具结果不再只剩一根文本。** image/audio/resource/structuredContent 六类块整链接入:严格验身(MIME 白名单+魔数复核)、三级字节帽、内容寻址落盘进会话 artifact;base64 不进历史不进 wire;旧服务端照常工作,新协议协商认不出明败。
- **专属浏览器 MCP 落地。** 常驻 Playwright 服务进程,十一件工具:独立 profile 带进程锁、页签与 ref 体系(导航后旧 ref 明报失效,不乱点)、Cookie 跨场、下载账、崩溃后终态清楚;Chromium 与 WebKit 双引擎,动作单队列串行,会话结束杀净子进程。
- 真浏览器矩阵 88 项(含取消 1 秒收口、双开明拒、杀进程终态)连跑三遍全绿;多模态模型真机看图一项无凭据,如实记未验。

## [v0.26.73] - 2026-08-28

- **项目插件信任一键批。** `/plugin trust <id>` 回执亮五样(版本、目录、工具清单、文件数、完整指纹)再落账,重启即挂;`untrust` 销账;未经信任的警告直接指路命令,不再让人手改 json 凑 64 位指纹。示例 README 补"装到哪"一节:用户级免门即用,项目级过门适合团队仓共用。
- **浏览器会挑下拉了。** Browser MCP 补 browser_select(按 value 或 label 匹配,选不上把候选清单整个回给模型);快照里下拉行自带选项单。真机验过当年卡死模型的路,四步走通。
- **Browser Agent 示例齐活。** SKILL 操作纪律(快照→ref→复验、stale_ref 应对、停手线)、安装排错 README、本地练习页;真模型全链跑通。六笔同类工具缺口(读区域文本、文件上传、通用按键等)已记账待续。

## [v0.26.74] - 2026-08-28

- **模型能看见自己工具截的图了。** 工具结果的图片整链回喂:anthropic 与 openai-responses 按各家协议原生真发,Gemini 3+ 走 inlineData,不支持的 wire 明说降级并把存盘路径给模型;会话档只存引用不膨胀,每轮从 artifact 重灌,前缀缓存不断。
- **process 插件协议升 v2,结果可带图。** 截图工具直喂模型(真机底牌剧本:随机口令与画面句子三问全对);v1 旧插件照旧,大小与魔数护栏同 MCP 规矩。
- **窗口清单修出真瞎点。** 窗口明细原来只进结构化附件被投影规矩吞掉,模型只见"共 N 个"一行;现正文逐窗一行带 id/标题/进程/状态/矩形,起程序后轮询找窗的纪律写进 Skill——记事本打字全程盲路真机跑通。

## [v0.26.75] - 2026-08-28

- **GUI Agent 学会"看结构"了。** 新工具 gui_snapshot:ctypes 直调 Windows UI Automation,把控件树文本化(按钮/输入/下拉各带名字与矩形,ref 短码定位),点按钮不再依赖看图——真机零截图纯结构路点中提交按钮并复核结果行,44 秒 7 请求。
- **三条腿走路。** 结构路(快照定位,首选)、视觉路(截图回喂,复核手段)、盲人路(窗口清单文本)各有分工;自绘控件 UIA 看不见的盲区如实教模型回视觉路。
- 快照三顶帽子防刷屏(深度/元素数/时间),超帽提示收窄;夹具控件名由程序自写,截图零侵扰。

## [v0.26.76] - 2026-08-28

- **大图不再炸预检。** 图片 token 预检从"base64 字节数当 token"改为按像素折算(宽x高/750,三处口径统一),3072x1918 整窗截图从虚报 65 万 token 降到 7856;整幅截图长边超 1568 自动步长降采样再回喂——用户真机"截图后追问必炸"的病根两层修死,追问轮复跑正常。
- **GUI Agent 纯视觉化。** 截图新收 region 裁切:局部原像素直出、不过降采样帽——裁切即无损放大,小字看不清裁一块再看;SKILL 重排为"先学看(list→focus→截图→裁局部),再学抄近道(UIA 降为捷径)"。
- **桌面常识七条入册。** 任务栏可能自动隐藏(先把鼠标移到屏底停半秒再裁横条看)、托盘溢出藏 ^ 后、UAC 置顶拍不到请用户按……句句"先做动作,再截图确认"。真机验:模型自己 hover 唤出隐藏任务栏,报出 15 枚图标。顺手把整屏截图从 10 秒修到 0.2 秒。

## [v0.26.77] - 2026-08-29

- **自定义 Agent 落了第一层。** Agent YAML 契约冻结(`docs/reference/agents.md`:逐字段、严格解析、诊断码、YAML 与 C++ 对照);builtin、user、project 三层扫描进 Catalog,`/agents` 一条列全账(来源、可用性、遮蔽),`/agent doctor` 逐份指错到字段与行号——坏定义标 unavailable,不炸全局。
- **general-purpose 与 Explore 登进 Catalog。** 纯只读面,现有行为一字未动。

## [v0.26.78] - 2026-08-29

- **Package 落了第一层(只读)。** package.yaml schema 1 契约冻结(`docs/reference/packages.md`:标准目录、命名空间、wire 百分号编码);严格解析指错到字段与行号,四层扫描(`--package-dir` > 项目 > 用户 > 官方),稳定盘点加整包内容哈希——改一个字节哈希就变。
- **`/package list/show/doctor` 三件只读命令。** 同名包报遮蔽账;symlink 与 junction 越界一律拦;近似目录名提示疑似拼错。阶段 1 不挂载任何组件,代码组件零执行。

## [v0.26.79] - 2026-08-29

- **workflow 的 agent/tool 节点接上真审批。** needs_confirm 工具(edit_file/run_command 一类)不再被一律明拒、更不诓称"用户拒绝"——确认直通主回合同一颗脑袋:档位与预放行先裁定,yolo/auto 不多问;真要问时 diff 预览、三档菜单、"总是允许"落回会话账。没接审批宿主的宿主里,拒词也改成实话("未接审批宿主")。
- **ask_user 增 override_answers(墨敕)。** 审核方已驳(review_approved=false)时,命中此档皇帝仍可越权放行:approved/complete 直接为真,不掺和 approve 的账;overridden 键恒在,下游直接读。
- **三省六部示例重写成 2.0:官制归数据。** 几案并陈(inputs.lanes)与职官路由表(inputs.ministries)都是数组——添一部、添一案,改输入不动图;谋议出 loop(献策一次,修诏多轮),门下不见用户原话、只审诏书里的"需求复述",御前加签与墨敕两特权落地。

## [v0.26.80] - 2026-08-29

- **两个扩展示例改造成 Package。** browser-agent 与 gui-agent 从 examples/agents/ 迁进 examples/packages/——按 schema 1 摆目录(package.yaml + skills/ + plugins/ + assets/),`/package doctor` 双双 valid 零警告;gui-agent 如实挂 code-bearing 标,被发现不挂载,过信任门才执行。git 认成 rename,历史没断。
- **examples/agents/ 改放散装 Agent 定义示例。** 新示例 code-reviewer.yaml 逐段带注,演示 allow 白名单、requires 断言与 runtime 取舍;整包能力示例一律指路 examples/packages/。

## [v0.26.81] - 2026-08-29

- **自定义 Agent 的 permissions.mode 对齐契约。** 解析器只认 inherit/confirm/auto/yolo;写了 read_only 会点名报错并指路"只读用 tools.allow 白名单表达"(带行列号)。内置 Explore 撤掉 read_only 标记,只读仍由五枚工具白名单扛——契约与代码不再两张皮。

## [v0.26.82] - 2026-08-29

- **自进化闭环落了第一层:观察与去重。** 五路只读 adapter 从 /record、Workflow run、/goal、ToolTrace、Memory 的既有账本收经验——只收有凭据的件(半截录制不收、裸成功不收),脱敏复用既有设施,思考原文压根不进账。
- **同类经验指纹分得清"哪一次"与"哪一类"。** 日期、URL、路径、入参原文、各家 id 不进指纹,归一为占位;重采幂等,同源同 id 不重复落账;被拒 fingerprint 留好去重口待阶段 2。
- **`/evolve status / list / show` 三条只读命令。** status 报五路采集与落账,list 按同类簇列观察,show 从一条观察指回原始账的文件与来源 ID——全程不生成 Package,不装任何东西。

## [v0.26.83] - 2026-08-29

- **自定义 Agent 真能派出去了。** 病根是派发层写死只认两个内置名,`agent_type` 写自定义名直接被打回、模型只好退派 Explore——YAML 里的预算与白名单全成一纸空文。如今 AgentCatalog 接进派发链:工具白名单、Skill 预装、runtime 预算、执行档与隔离档都从 YAML 落地,改 YAML 即生效。
- **子代理三线预算真执法。** 步数、时间、token 各有硬线,断线即收场带部分结果;软线(默认 80%)先注入一条"请基于现有证据收尾"。超预算不丢工作树,结果附房路径与断线缘由。派发参数也收 `max_time_secs`、`max_tokens`。
- **Dock 不再冒名。** 自定义 Agent 按 resolved 名显示(`library-reviewer #1`,不再 `Explore #1`),截断只截任务摘要;坞行常驻步数,详情带步数/时间/token 预算明细,台账、Dock、日志三处一个名。

## [v0.26.84] - 2026-08-29

- **压缩不再反涨、不再连环压。** 病根三股:热区把 mid-turn 巨型工具循环整轮吞下(post=pre+存档)、触发线拿未压缩全量估(比真实请求虚高四倍,误触发)、无滞回。今治:热区真预算(末轮超 12k 时按工具来回从尾收、配对不劈),换账前反涨闸(新史不比旧史短即拒收、历史一字不动),4096 token 滞回带宽(上次收口后新增不足即不再压,摘要请求都不发)。
- **上下文三本账合成一本。** loop 的 projected、`/context` 的估算、压缩前后账全走同一个 dry-run 视图,与真实请求同一把尺——不再出现"账面 70k、真请求 47k"的两张皮。
- **致命退出不再无声。** 顶层补兜底 catch,退出前写错误类别、会话存档目录与 `--continue` 指引;压缩后的下一次请求、工具执行、session flush 有回归钉。顺手修实一只真 bug:压缩事件的保留集改带 kept_ids 跳跃集,`/resume` 不再把裁掉的组又装回来。

## [v0.26.85] - 2026-08-29

- **问话面板开了多行,画敕不再盲批。** ask_user 菜单的问题区原先只画一行,整份方案书塞进去只剩个标题——御前批的是盲敕。如今按屏宽折行、行帽钳制(屏高一半,3 至 12 行),超帽截断带省略号;画敕的问话改摆一句话摘要加门下判词,方案书全文走面板条目与 /workflow history。
- **map 并发各路不再串账。** 谋议三案、六部办差并发跑同一节点时,事件账里三路挤一枚条目互踩(信息重复又串线)。如今 node_run_id 带路号,面板各路各一条、行尾挂"第N路";面板里的 JSON 正文收口时缩进摆平,不再一行糊脸。
- **main 不再倒 workflow 节点的原始 JSON。** 与子代理同规矩:main 只看阶段页脚与收官摘要,节点内幕留在 Agent 面板——机器话不刷屏,人话留着给人看。

## [v0.26.86] - 2026-08-29

- **粘贴收成一次事务,重绘走脏行。** 一万字中文单批粘贴从逐字万帧(约 2MB 刷屏)收到 9 帧 1867 字节;bracketed paste 的 `ESC[200~` 标记剥净不再逐字漏进正文;正文流式增量只清压到的那几行,行内增量零字节输出,Plan 大重绘不再整框擦了又画。
- **收口符不许独自掉行。** `/plugin doctor` 的 `node(v24.0.0)` 断行病根是 `--version` 尾换行嵌进格式串——版本串取首行剥白,折行按 ANSI 零宽与中文两列算宽,右括号与中文标点不再起行打头;80/100/窄终端快照钉死。
- **帧账可审计。** `LUBANCODE_FRAME_AUDIT` 开关把帧数、字节、光标落点写 stderr;conhost 刮屏驱动 21 项与流式页脚 G7 幕全过,管道重定向无 ESC;Windows Terminal 无注键 API,如实记未验。

## [v0.26.87] - 2026-08-29

- **Plan 门只拦改,不拦读。** skill 加载与派只读 Agent(内置 Explore,自定义按 tools.allow 全只读判)放行;git 补 ls-tree/rev-parse 等一族只读子命令,PowerShell 补齐管道件,`Get-ChildItem | Select-Object` 与 `git ls-tree` 不再吃闭门羹;脚本块 `{ }` 体内是任意代码仍拦,拒绝回执把命中的规则印出来。
- **子代理不再吞自己的日志。** `.evidence` 与调试日志目录进观察边界,search 默认不搜、read_file 默认不读;逐字点名才放行,读前报体积,超 256KB 劝阻并指路 offset/limit——递归膨胀(日志装日志)有回归钉。
- 浏览器服务拆三层:session(Playwright 生命周期与全部状态)、tool adapter(纯组装)、transport(纯分帧),server.js 从 1137 行瘦到 220 行;104 项自测零回归,新增 Runtime 直调路 130 项两遍皆绿,阶段 3 的 App Server 适配有了桩。

## [v0.26.88] - 2026-08-29

- **Prompt Profile 三层覆盖成真。** 内置/用户/项目三层稀疏覆盖同一模块树,后者压前者;项目层只在选中 Profile 时参与,主 Agent 的 default 拼装一字不动(黄金测试零 diff,数学与字节双证)。内置样例 `browser-tester` 随库。
- **能力说明不再说一套做一套。** 自定义 Agent 的 feature 文案从过滤后的有效工具表推导——没给 shell 就不装 shell 说明,没给 todo 就不装 todo 说明;web/mcp/lsp 只认能力,不吃父会话开关。
- **`/agent inspect` 点亮来源账本。** 定义来源、覆盖链、prompt 三笔开关、逐模块"这段提示词从哪层哪文件来"一目了然;`/agent doctor` 的 Profile 行升级为查三层覆盖存在性。

## [v0.26.89] - 2026-08-29

- **`/plugin test` 真跑测试了。** 发现插件目录同位的 test_runner.py/.js/.mjs,用 manifest 自己的解释器起进程,回执带入口、命令行、退出码、耗时与 stdout/stderr 摘要;起不来、超时、非零退出分层定位。真机验:gui-agent 55 项自测 479ms 通过,故意失败的探针带完整 traceback。
- **三份命令名单同源。** `--help`、`/help`、Tab 补全全从 cli::AllSlashCommands 出,手抄清单撤下——/plan、/agents、/agent 不再只在补全里见得着。
- **小件四桩清账。** TODO 回合收口时提醒未关项(打断与预算断线不催);C4099 class/struct 前置声明清编归零;测试 FindEvent 加右值删除重载,悬垂写法编译期就报;Agent YAML runtime 段补齐契约五字段(带下界上限校验)。

## [v0.26.90] - 2026-08-29

- **三省六部发牌改按单序串行。** 真机实翻一桩设计病:发牌并发跑,刑部要记"修复前的红基线",工部同一时刻正在改代码——基线没记着,病已治完,复命只好如实报"部分办成"。验-修-验有先后依赖,同一工作区不许两只手同时动:发牌改 foreach 串行,尚书拆差遣时写明数组即次序、有依赖的差事排先后;差役回执头一行署部名,御史归账有据。

## [v0.26.91] - 2026-08-29

- **PowerShell 脚本块的洞全堵了。** `where-object { del x }` 这类命令,黑名单原先只查首词就判 Safe——脚本块体内是任意代码。今下沉一份检测进 command_safety,Plan 与 Auto 两档共用;Auto 档的白名单与放行账两条暗路也前置同一道闸,命中拉回询问。yolo 与"总是允许"的显式全放照旧不拦(与 deny 待遇对齐)。引号内的 `{` 不误伤,cmd 无此语义不查。
- **turn_view 高并发假红治净。** 心跳两处吃墙钟的断言改限时重试(8 秒内满意即收),`-j 8` 三连跑零红;真坏了轮轮红,调度抖动至多多跑几轮。

## [v0.26.92] - 2026-08-29

- **Package 会看组件了(阶段 2,仍只读)。** 六类组件(Skill/Workflow/Agent/Prompt/plugin.json/mcp.yaml)一律过原生 parser 解析,包层零 schema 复制;`/package doctor` 列组件清单、引用解析全账与静态 MountPlan——哪件挂到哪张表、wire 名与展示名各是什么、依赖谁,一眼看清。不启动 Plugin 与 MCP。
- **包内短引用成真。** 组件互引先在本包找,包外须写 `<包id>:<名>` 全名;悬空引用、wire 名超 64 帽、路径越界(`${package_dir}` 逃包根)任一命中即整包 invalid——但全件照解析逐件照报,不因第一个错停摆。
- workflow 定义补收 `agent` 字段(包内短引用的钦定落点,旧 `role` 不动);工具 wire 编码收进 plugin_contract 一处(Encode/Decode/64 帽)。

## [v0.26.93] - 2026-08-29

- **自进化闭环转起第一圈:一场 /record 能变成候选了。** `/evolve propose` 从录制件起草 Skill-only 候选——正文出自现有起草器,偶然值抽象成占位,稳定失败路写进排错节;封成最小 content-only 包(过 manifest 严格解析),落 `~/.lubancode/package-candidates/` 带全套演化账(evolution.json、approval、状态迁移账)。
- **`/evolve diff / reject` 接上。** diff 与父版(或空)对照列新增件与正文摘要;reject 落拒绝指纹进去重账——同类经验不再反复劝起草。候选只进候选区,`/package list` 看不见(有反证单测:拷进用户层立即扫得着,证明是分仓不是不合法)。

## [v0.26.94] - 2026-08-29

- **浏览器学会记账了:Console 与 Network 两本 journal。** 每页环形账(帽 500 可调),条目带页内单调 seq 与 generation,断线可补账;console 文本与 network URL 敏感键值自动遮 `<redacted>`,超长截断。两枚新工具 `browser_console`、`browser_network` 按级别/URL/状态过滤查账,关页后账仍可查。
- **用户动过手,旧 ref 即作废。** 仲裁第四条真落地:页面挂输入监听递 userEpoch,Agent 拿旧快照动作时明报 stale_ref 并注明"用户在页面上动过手";Agent 自家输入不误记。浏览器自测 130→162 项。
- **截图进 App Server 事件。** 工具完成事件附 images 数组(MIME/尺寸/哈希/artifact 引用),绝不带 base64;纯文本工具不带该字段,老事件形状不变。headed 模式真窗冒烟过,CI 无显示器如实 SKIP。

## [v0.26.95] - 2026-08-29

- **AgentProfile 解析归一处。** 新 AgentProfileResolver 成为唯一合并权威:父 Agent、定义 YAML、模型角色、Prompt、Skill、工具、MCP、权限、runtime 九路材料六步合成一份——散在派发链里的手工合并尽数收编。同一份定义从 AgentTool 与 Workflow 两条路解析逐字段对账,连"错也错得一样"都钉了测试。
- **权限只可收窄成铁律。** 子 Agent 想比父档宽(confirm 想跳 yolo)即结构化明拒,不进连败账;requires 缺依赖、allow 点到父面之外,各报结构化错不悄悄放宽——后者头一天实战就逮住一处旧测试债。
- **YAML 预算四字段并流。** max_output_tokens 等 runtime 预算从定义 YAML 真正落进运行时(显式 > 父值,来源档随级标注),`/agent doctor` 与 `/agent inspect` 亮出来源账;请求期拼装(黄金测试)零变化。

## [v0.26.96] - 2026-08-29

- **自定义 Agent 全路对齐内置(阶段 4)。** 写死的类型白名单拆除,派发一律查 Catalog——查不到报"没有这名,看 /agents";agent 工具的类型说明动态列出当前可派的人(名字加一句描述,缓存一回合一刷,不拖请求);用户/项目层覆盖内置名首次真生效。前台、后台、取消、消息、结果回收、worktree 隔离逐件对账钉死。
- **权限收窄真执法了。** 子定义比父档严时(父 yolo 子 confirm),确认下限一路带到裁定口——yolo/auto 的免问不再免,该问真问;`--yes` 显式全放与 deny 黑名单的次序不动。
- 真机冒烟三幕全过:主请求 schema 列出 smoke-reviewer、按名派出、子代理工具面与说明面双收窄——模型按描述挑人这条路通了。

## [v0.26.97] - 2026-08-29

- **候选要过五道门了(评测与基线)。** 静态门复用 Package doctor 另补密钥与绝对路径扫描(占位写法不冤枉);来源回放与留出任务走确定性检查器(文件存在/JSON 可解析/内容命中/命令直起不经 shell);基线对照比父版或裸 Agent,七项指标账逐行落。静态门全过迁 validated,五门入账迁 evaluated——都经 Coordinator 唯一写口,候选改一字哈希对不上即拒评。
- **`luban evolve test --json` CI 路。** 全过退 0,验收挂退 1(记退出码与"低于基线"对照),夹具缺失退 2(明写"没测,不是测砸");JSON 带逐项 checks、各门 outcome、unverified 清单与基线七路 delta。`/evolve show` 同页出"通过几项、没测什么、比旧版贵多少"三样。
- **判不了的老实说判不了。** tokens 恒 0(不起模型)、误报漏报、模型在环、真实服务、兼容范围——五项 unverified 明记,不拿确定性结果冒充模型评测。

## [v0.26.98] - 2026-08-29

- **Package 的内容组件真挂上了(阶段 3)。** Skill、Workflow、Agent、Prompt Profile 四类从包根落进各自运行时表,一律 canonical 名(`<包id>:<名>`)登记展示;放包进目录,重启后四类同现,拿走同隐——端到端测试钉死。Plugin 与 MCP 照旧一件不挂不执行,等阶段 4 信任门。
- **包层座次定死:本地主人盖过第三方包。** Agent 为 project > user > package > builtin;Profile 的 canonical 名只在包根解析,裸名五层一字不动(黄金基线续钉);Workflow 包层不抢裸 alias——许诺一个直呼 alias 会骗人,canonical 正门才通。
- **legacy 散装零变化有逐字节证据。** 无包时技能扫描、AgentCatalog、workflow 目录、系统提示词拼装与旧路完全一致;会话钉快照——盘中途增删,会话内不变,下回启动才见。

## [v0.26.99] - 2026-08-29

- **App Server 有了浏览器协议(1.1,additive)。** 方法十八枚(会话/页签/导航/快照/截图/动作/journal 查询),事件十三族全带连接层 seq;真 Runtime 住 Node sidecar——懒起、整台复用、崩了发事件并收口在飞、下笔自动重起、收线杀进程树,Playwright 生命周期仍只有一本账。独立协议客户端 26/26 真跑:控页、收事件、断线边界。
- **高频事件有界可合并可补账。** journal 源头批量(帽 200、40ms 一冲、撞帽丢整批计数),出站同页合并;丢了凭 sinceSeq 补账,dropped 明说;截图只发 artifact 引用,整条出站流无 base64。
- **浏览器动作过审批与取消。** agent 动作须带 threadId 过 permission/request,acceptForSession 按方法记会话级放行;取消贯通审批段(悬空收口、迟到答复明报)与执行段(轮询见旗即停),thread/stop 联动。顺手修净三处 app_server 既有暗坑:stdin 短报文阻塞、出站无写线程、审批答复 resolve 口没递——交互客户端原本立不住的根。

## [v0.26.100] - 2026-08-29

- **官方浏览器包 luban.browser 0.1.0。** browser-reviewer Agent(只读面、预装包内 Skill)、四只 Workflows(smoke 冒烟/responsive 双视口/console-error 容忍清单/network-failure 关键请求)、练习页三课靶子;Skill 与练习页从旧 browser-agent 包整体迁入,git rename 保历史。
- **包只装章法,不装浏览器。** Runtime 归核心(MCP 配 config、协议 1.1 在核心),包声明依赖不复制本体;可选 code-bearing 的 MCP 薄启动器(找不到核心服务明说退出,不下载不内置)。卸掉包,浏览器照用——少的只是专门 Agent、Skill 与 Workflow,doctor valid 七组件引用七十条全闭合。

## [v0.26.101] - 2026-08-29

- **Package 信任门立起来了(阶段 4)。** 指纹算法从 Plugin 信任抽成共用底座(旧 plugin-trust 账一枚不失效);PackageTrustStore 键绑包 id 加整包内容哈希——改一个字节旧账自翻脸,show 指路重批,改回去旧账重新对上。user/official 层放置即信任,project 与 dev 层要批——与 /plugin trust 同尺。
- **`/package trust` 五样回执。** 插件命令 argv 直递不经 shell、MCP 的 command/args/env 形状(env 只认 `${env:NAME}` 占位,真值不落账)、网络面、文件数、完整指纹。未过门:content 组件照挂,plugin/mcp 一件不进挂载账。
- **依赖连坐。** 未信任包里直引 plugin/MCP 的组件先倒,引用倒者的 Workflow→Agent 跟着倒——`/agents` 里明注"不可用:依赖的 mcp_server 未过信任门",不静默。

## [v0.26.102] - 2026-08-29

- **Workflow 节点认得自定义 Agent 了(阶段 5)。** `agent: <名>` 三层解析:包内短引用折 canonical、宿主口查 AgentCatalog、resolver 出统一材料;回执与 journal 事件的 agent 键落 resolved 名。同一 YAML 从 agent 工具与 Workflow 节点两路各跑一轮,系统提示词与工具表逐字节一致——同源有对账册钉死。
- **编译期拦鬼名。** validator 新增 unknown_agent(capability 表从 AgentCatalog 现扫),写错名字编译就红,不静默换 general-purpose。权限下限也接进节点:自定义定义比会话严时,该问真问。
- 前台失败、预算尽、取消、后台回收四件语义与内置路逐件对账;顺手修实 0.26.92 一笔真欠账——定义 JSON roundtrip 丢 agent 字段,会伤 journal 恢复与 ContentHash,已补齐带测试钉。

## [v0.26.103] - 2026-08-29

- **自进化闭环走通了全程(阶段 4)。** propose→test→approve 出批准页(id、版本、哈希、来源、改动、评测摘要、权限差异、安装位置、灰度办法、回滚目标十样)→use 点名 canary→promote 转 active→rollback 一条命令切回父版。批准只认当前哈希:改一个字节,评测、批准、store 三道门先后拒之。
- **version store 原子落、账一枚不删。** 候选复制 staging→复算哈希→再过静态门→rename 成正式版本(`~/.lubancode/package-store/`);写一半失败正式 store 纹丝不动。回滚只切指针不删版本——评测账、批准账、迁移账、install-log 全保留。会话钉快照:promote/rollback 后旧会话照用旧版,新会话拿新版。
- **store 选中版本并进 `/package list`**(scope 记 store,压 user/official、让位 dev 与 project)——进化出的能力正式进了发现面;手改 store 文件,下次启动拒挂并指路。

## [v0.26.104] - 2026-08-29

- **挂载事务落地:整包成整包败有了执行版(阶段 5)。** 过了信任门的 plugin 与 MCP 先进暂存——插件起探针进程走一遍协议(判通道不判业务),MCP 走 initialize+tools/list;全部起得来才原子并进正式 ToolRegistry,一件起不来整包回滚(杀已握手的进程、清暂存表、零残留),诊断指到坏件。两插件一 MCP 三件套测试:坏一件,三件全不进。
- **工具带上了出身。** 每件包内工具挂 ToolOrigin(package/version/component),`/tools`、`/plugins`、`/mcp` 照实显示"来自哪只包哪一版";占位符 `${package_dir}/${package_data}/${env:NAME}` 结构化展开,逃包根双闸拦。
- **修实一桩合并事故(致歉)。** 0.26.103 的合并把三处未解冲突标记提交进了 package_commands.cpp,当夜的"全绿"吃了陈旧二进制的假账。本版解净(信任账与 store 五路两边都留),以新编译产物重验:241 册全绿,exe 时间戳亲核。

## [v0.26.105] - 2026-08-29

- **gui-agent 学会两件结构路兵器(借 computer-use 的招)。** `gui_set_value`:UIA ValuePattern 整体替换可编辑控件的值——先校验可写、存替换前值、设完复读核对,比逐字 typing 可靠;`gui_invoke`:InvokePattern 点按钮、ExpandCollapsePattern 展开收起下拉树形,做完回读真状态,不冒充。
- **快照自带路标。** 控件行尾标 `[value]` `[invoke]` `[expand]`,模型看得见哪条结构路可走;SKILL 纪律同步——优先结构路,坐标与 typing 降为后备。ref 解析抽成公共底座,快照与动作同一套折叠规则与 DFS 序号,深度帽错位有专案钉死。
- 自测 60→73 册(活体册用原生控件——tkinter 三只 pattern 一只探不出,这课写进文件头存照);BSTR 指针不钉 restype 会被 c_int 截断成残指针直接崩,探针踩实立注。

## [v0.26.106] - 2026-08-29

- **进化学会提炼组合包了(阶段 5)。** 两把尺分档:成功路同形(≥2 场独立任务、折叠后工具序列名字次数次序全同)起 Workflow;全场工具面也同形(含失败尝试摸过的工具)才添 Agent——失败重试里的习惯也算这只角色的面,面不同不封同一只 Agent。一把宽一把严,中间那格有专案钉死:两场成功路一致、一场多摸过一件连败工具,Workflow 照起、Agent 不添。
- **组合包过不了静态门就降档。** 引用悬空、canonical 名不合规,删掉 workflows/ 与 agents/ 降回 Skill-only,诊断带进迁移账,哈希按降档后形状复算——不硬塞。异值入参提成 `${inputs.*}`,各场示例只进描述不焊死。
- **评测分家在解析层硬拒。** acceptance 的 kind 白名单没有"跑被测 workflow"这一种,认不得的 kind 整份计划拒解析;评测账带复杂度代价栏(shape/组件数/多出的维护面),批准页明写"批的是这份代价换来的编排"——不是组件越多越容易晋升。

## [v0.26.107] - 2026-08-29

- **Package 体系收官(阶段 6)。** 启停账落包外(`~/.lubancode/package-state.json`,原子写):停用的包扫描发现照旧、挂载一件不挂;`/package enable|disable` 回执明说生效时机。`/package reload` 原子重折——折好才换档,折不动旧账一分不动,回执带增减改对账;code 组件须新会话,不热插不卸载。
- **会话钉快照升成真身。** PackageSnapshot 显式对象(路径、哈希、整份解析好的组件定义、技能正文查表):在跑的 Agent/Workflow 钉各自那份,盘中删包改 SKILL 都影响不到;reload 后新装配才见新账。回归钉死三条:半场 Workflow 不换 Skill、reload 不卸 ToolRegistry 正引用的模块、目录突变不伤在跑引用。
- 至此统一 Package 封装七阶段全通:发现→校验→记账→挂载→卸载,一只箱子该有的都有了。

## [v0.26.108] - 2026-08-29

- **排队命令不再被喂给模型(实测问题 2,P1)。** 忙碌期提交的斜杠命令保住命令身份:工具边界让路不注入,轮末经本地分派器执行——`/context` 真开面板,模型请求里查无它的字样。身份由正文现折(与 ProcessLine 同一颗判头),存档、resume、取回改写天然跟正文走,零格式改动。
- **十九案放行、三类明拒。** 只读面板与可逆维护类(help/context/todos/compact 一族)可排队轮末执行;菜单向导类、换场毁档类、起工作类提交即明拒并留正文供改写,不悄悄降成普通消息。混排各走各路:文字工具边界照旧 steering,命令轮末本地执行,次序语义有单测钉死。
- 回归专钉"工具边界早于轮末":带工具回合中途塞 `/context`,三次模型请求查无、轮末面板真开。

## [v0.26.109] - 2026-08-29

- **AppServer 长出了 WebSocket 腿(多前端阶段 A)。** `lubancode app-server --app-server-ws <端口>`:同一套协议与 dispatcher,新一条 WS 传输——自写千行 RFC 6455 极小服务端(握手/帧编解码/回环 TCP 薄层),零新依赖。断线只收连接不收进程:thread 账与浏览器 sidecar 跨连接存活,重连凭 cursor 补账,seq 永不回卷。
- **token 门。** 非回环绑定没配 token 当场拒启;配了即走首帧 auth(恒时比较,不落日志)。stdio 与 WS 按需起一种,语义各有各的账。
- 独立手搓 WS 客户端(零 npm 依赖)29 项全过:token 门、thread/turn 真回合、断线重连补账、浏览器面十三件(含审批反向请求、截图 artifact 无 base64、journal 全量回放)。Tauri/Web/Android 外壳的敲门砖垫好了。

## [v0.26.110] - 2026-08-30

- **流式粗体真渲染了(实测问题 1)。** 根子两层:正文段后直接跟工具调用时,工具边界只清账不重画——整块正文永不渲染、星号永久露白;段内 `**` 闭合后也苦等段尾才画。今治:块内增量重画(每新到一行或行内标记配成一对,按累计正文整块重画,防洪峰有预算帽)+工具边界让路定格(未收束块先按渲染版定格再清账)。
- **转义补齐。** 行内转义从只认 `\$` 扩到 `\*` 与 `` \` ``——`\**` 不被吞成粗体,代码块内字面量原样保留。九册新测试穿真实分块收束路径(单块/跨 delta/逐字/混排/窄终端/预算退场),刮屏驱动全过:全屏无裸星号、粗体真带亮色。
- 排查明账:同链路的标题/列表/行内码一并修好;链接 `[文字](url)` 是独立既有缺口(未动,记账);问题 3(标题前空行)判明**不同根**(push_blank 块首间距),另单治。

## [v0.26.111] - 2026-08-30

- **缓存面板不再拿"模型请求"冒充"轮"(实测问题 5)。** 每笔缓存记录带 turn_id 与请求序,`/context` 按**用户轮次分组**——组头人话标签(轮次首行),组内逐次模型请求列输入与命中;"用户轮次""模型请求"各叫各名。表头明写"仅主会话、会话内最近 12 次、跨用户轮次";达上限明说只留 12、全会话总数另有账;provider 未回 usage 标"未回报",不冒充 0%,也不整行蒸发。
- **分角色账列全三行(实测问题 6)。** cheap/normal/lao 固定全见,零调用写"0 次 · 本场未触发(职责句)";回落 normal 的角色行内写明来源——与 `/model roles` 同一张路由表同一份口径,不再让 lao 凭空消失。
- 一请求五工具的冒烟实录:"覆盖 1 个用户轮次 / 6 次模型请求",分组逐笔可追。

## [v0.26.112] - 2026-08-30

- **后台任务有了用户的管理面(实测问题 4,P1)。** `/background show <id>` 出九项详情;`logs <id> [--tail N]` 读日志尾巴(空/删/坏/超长/已退出各有一句如实话);`stop <id>` 与 `stop all`(列单确认、终态不重复杀)全部本地执行不经模型。停止走三段(运行中→停止中→已停),整棵进程树收净才报停,失败带原因不假报成功。
- **底栏常驻后台计数。** "后台 N 运行 / M 完成"每帧现折,无任务收起;任务进终态那一刻主循环当拍醒来打通知,台账留 200 条随时回看——模型忘了收尾,长命服务不再人间蒸发。
- **顺手治了一只底层病**:后台 PowerShell 原先套捕获式 wrapper,输出憋在内存、进程不退日志零字节——dev server 被停,日志全陪葬。今改裸执行边跑边落盘,退出码契约四分支实测钉死。

## [v0.26.113] - 2026-08-30

- **标题分两层,首问即见(实测问题 7)。** 首条 query 建档成功当场出本地标题——取首行剥围栏截 24 字,零模型 token,不等首个回复;精炼请求紧随其后异步发(只喂首问 600 字节、输出帽 24 token、5 秒看门狗、无工具),回来原子替换。十分钟的任务不再等十分钟才见会话名。
- **提示符不再被标题卡住。** 旧路轮末同步等 6 秒+;今独立线程持独占 backend,与主请求不共 client 不抢回调——冒烟实录:主答 1.4 秒落屏,cheap 挂 6.5 秒照跑,轮末零等待。未配独立 cheap(回落 normal)时一个请求也不发,留本地标题;人工 `/title` 翻代优先,迟到的自动结果丢弃但 usage 照记。

## [v0.26.114] - 2026-08-30

- **标题前的空行回来了(实测问题 3)。** 病根:分块渲染把前一块块尾的分隔空行一并擦掉,后一块又自带不了前距——列表末项与标题贴死。今治:前距随块走(上一块已定格成渲染版时,本块渲染前垫恰好一行;原样保留或开头不带,免双行),空行连发在落笔层吸收成一行。不破"按空行分块"的架构,整篇渲染路一字未动。
- 测试立了条铁律:流式分块的**最终画面逐行等于整篇渲染**——单 delta/跨 delta/逐字三种到达方式,段落/列表/代码块接标题都恰好一行间距,开头标题不凭空多空白。刮屏驱动 17 条全过。

## [v0.26.115] - 2026-08-30

- **`?` 帮助能收起了(实测问题 10)。** 帮助表做成底栏帧行(垫最顶、进指纹进高度预算,装不下保头舍尾),展开覆写可视对话不写滚屏;再按同键或 Esc 整屏重建收起——连续开合十次滚屏零残留。文案终于与实现一致。
- **开合是台纯函数状态机。** 四事件(翻面/Esc/草稿有字必收/场景让位必收)各处只报事件不定规矩;打字、粘贴、取回、转录导航、查看态切换都随草稿与场景走——两块面板同一本帧账永不叠画。表头表尾显示实际和弦,改绑 `alt+h` 后照实写;非空 composer 的 `?` 仍是普通字符。
- 顺带修掉一只老雷:底栏帧收小时 diff 不认旧帧范围会留鬼影(队列抽条同款病根),刮屏 64 项两轮全过。

## [v0.26.116] - 2026-08-30

- **Ctrl+R 不再把每条提问列两遍(实测问题 8)。** 提问事件有了稳定身份(session id + 场内序号):磁盘与内存两侧对上号即合并,同文不同事件(真发了两次)各自保留各带时间;活历史只补未落盘的尾巴,不再整场重抄。"这条消息算不算提问"归一成唯一判官,磁盘内存共用一份口径。
- **手动驱动器真按 Ctrl+R。** 进程内假服务+真 exe+真按键+刮屏计数:两条已落盘恰显两条、三号落盘后恰三条、同句两次恰两行、建档失败时未落盘尾巴照显——两模式 ALL PASS。顺手治了搜索面板一桩:开张那帧被"查询未变"短路,显"没有命中"要敲一个字才活,今 Open 即列全部命中。

## [v0.26.117] - 2026-08-30

- **缓存面板会回答"断在何处"了(实测问题 9,P1)。** 每次模型请求留一笔诊断账:cache_epoch 与断因、前缀追加律、稳定前缀条数与指纹、provider 命中数;miss 五档分型——未回报/首请求/断 epoch/**上游未命中(本地前缀稳定)**/命中。本地前缀稳定而 provider 报零时,行内明写"上游未命中",不再让用户猜是自己多发几条冲掉了缓存。诊断只落短 hash 与长度,不落正文与密钥;端点披露剥掉 query 与 fragment。
- **公网探针过确认门。** /doctor cache probe 对公网端点先亮牌——发几轮、每轮上限、目标端点,答 y 才发;回环与明配 metrics_url 照旧直发。多组分型五档判词:稳定命中/固定阈值命中(如恒 1024)/间歇 miss/完全未见命中,证据边界如实写。
- 顺手修 CI:POSIX 册 Register 调用补 cwd 参(0.26.112 添参时 #ifndef _WIN32 段没跟上,MSVC 编不到)。

## [v0.26.118] - 2026-08-30

- **轨迹事实仓打了地基(P0-1a)。** 新 `src/trajectory/` 模块七件:EventEnvelope 全字段与 67 种事件强类型(每 kind 钉死 plane 与三档 id 要求)、schema 强校验、CanonicalJsonDump(key 递归字节序排序,跨 Windows/Linux 字节一致,Grisu2 浮点)、内容寻址 BlobStore(临时→hash→fsync→原子 rename)、Journal(SHA256 hash chain、durability 三档、终态封口)、Recorder(单锁串行,18 条状态机硬约束提交时即时检查,违例回稳定错误码)、workspace/session 目录制。
- **golden fixture 钉死字节。** 19 枚合成全流固化为 v1 golden:新写字节逐字一致、同流两遍录制整本同 hash、旧读 round-trip 过;截断尾行 verify 明报、前面事件照常可数。37 册单测把 18 条约束各钉一案(含审批 deny 后拒、queue 双终态拒、session.ended 后拒写)。
- 这是 P0 轨迹工程的第一批(合同与最小仓);后续 SessionManager 换账、运行时单写口接线、replay/resume、训练导出依序接上。

## [v0.26.119] - 2026-08-30

- **Lua 插件的合同与钥匙仓打了地基(Lua HTTP 单阶段 0+1)。** `plugin.json` v2 冻结:manifest_version 版本墙(v1 只收 process、embedded-lua 明报需 v2)、NetworkPermission(https/精确 host/443/GET,POST)、SecretDeclaration、HttpLimits 六帽只许下调——13 枚稳定错误码带行列。宿主错误总表 17 枚照设计全表落。
- **SecretResolver 全套。** 环境变量 + 插件数据目录窄 .env(BOM/引号/CRLF 收,插值/export/多行/超 1MiB 拒,只装声明过的键);`SecretValue` 编译期禁复制、移动即覆写源、operator<< 删除误用即编译错;`SecretRedactor` 从长到短替换防截断。**源码树 .env 永不读**与**.env 轮换即时生效**各有专测。
- **假 Key 全链路搜不到原文**:解析→展示→错误文案→打码文本四路泄露扫描回归;inspect/doctor 只报名字与来源。punycode 是 RFC 3492 真实现(标准向量逐字验)。

## [v0.26.120] - 2026-08-30

- **Kimi 保留式思考契约接通(P0)。** `ReasoningReplayPolicy` 新增 `Always`:历史 assistant 的 `reasoning_content` 经 JoinedThinking 块序原字节回传,只认 provider 正式字段产出的 ThinkingBlock——`<think>` 正文、compact 摘要、宿主提示一概不伪造。方言驱动链贯通:模型级 dialect.replay/replay_field 裁决,旧 ChatRequestOptions 留作自定义 provider 回落。
- **Moonshot 四枚模型各归各位。** kimi-k3(effort only,low/high/max)、kimi-k2.7-code(+highspeed,零参数即思考)固定 Always;kimi-k2.6 只保本枚 Turn 工具循环(ToolEpisode,不发 reasoning_effort);kimi-k2.5 固定 Never 不误开。模型级方言全部标 verified;K3-fast 与聚合端不沾光。
- **loopback 集成钉死契约**:K3 两轮纯对话第二轮请求体断言第一轮思考原样在场;K2.6 工具循环不发 effort;K2.5 不回传不误发 keep。`/think` 分行亮"本轮思考"与"历史回传"两件事。src/ 无一处模型名特判(铁律核查)。

## [v0.26.121] - 2026-08-30

- **嵌套 AGENTS.md 管到目标文件了(P0)。** `ProjectInstructionResolver::ResolveForPath` 按机械序 root→最近层出结构化 chain(每份文档带 scope/sha/override 记号);写文件(edit_file/write_file)在 PreToolUse、用户确认、任何副作用**之前**过 `ScopedInstructionGate`——新 scope 首写拦下,规则全文随 tool_result 注入下一请求,模型重试放行;指纹登记认 scope 不认文件,规则一改旧确认自然作废。
- **多路径原子**:多目标写按 chain fingerprint 分组,任一 scope 未确认整笔拦,零副作用。主 Agent、子代理、Workflow agent/tool 节点、one-shot 全接同一只 resolver,各 Agent 自持确认账;基线预登记逐字节全等才认(搬房/手改/超帽都重新握手)。
- **旧行为零退化**:root→cwd baseline 与 AGENTS.override.md 优先照旧,既有四案原样绿;无 AGENTS.md 项目闸全程直行。集成测试真起 RunOneTool:从仓库根写 src/parser 下文件,首发被拦注入 override 压过 AGENTS,重试落盘;根下文件不受嵌套层影响。

## [v0.26.122] - 2026-08-30

- **受控 HTTP 真水管铺好了(Lua HTTP 单阶段 2)。** 中立传输底座 `src/net/http_transport`:完整 GET/POST、连接超时+硬墙钟、双回调取消、响应字节记账、DNS seam(生产 getaddrinfo,测试注假账)。五道网络边界全落:URL 解析禁 userinfo/fragment、scheme/host/port/method 命中声明、非 IP/localhost、DNS 候选逐枚分类(v4 15 段+v6 9 段,混一枚私网整体否决)、**连接钉已验地址防 rebinding**(cpr SetResolves 够用,不必降裸 curl)。
- **挖出一枚暗雷**:cpr 的 Session 构造器默认跟重定向(Redirect::follow 缺省 true)——不显式关,3xx 会静默跟还拿真 DNS 解析 Location,正好绕过钉地址。本单显式关死;provider SSE 路同患未迁,记账留后。
- **四处字节帽在数据入口落锤**:URL/请求头(Secret 注入后)/请求体在 cpr 前拒(拒时 DNS 都不问);响应头/体在回调里达帽即掐流不多攒一块;墙钟分型 timeout。取消五段覆盖,timeout 与 cancelled 不串码。回环假服务全矩阵(§13.3 逐案对号)含私网/metadata/rebinding 夹具全拦。

## [v0.26.123] - 2026-08-30

- **Lua 拿到了受控的水管与钥匙(Lua HTTP 单阶段 3)。** `luban.http.request` 与 `luban.secrets.available/ref` 注册进 manifest-backed Lua state;`LuaCallContext` RAII 钉死动态作用域——顶层加载期调任何 Host API 一律 `no_active_tool_call`,假 transport/resolver 计数为零,结构性碰不到网络与 Secret。
- **SecretRef 锁成黑匣。** 零字节壳 + 逻辑 id;`tostring` 只得 `<secret:id>`;索引/拼接/遍历走 Lua 原生报错;metatable 锁死;转 JSON 被拒(handler 想把 ref 回给模型即报错);`auth.secret` 字符串糖与 ref 同折同一条注入链。C 函数整件 try/catch,异常不穿 Lua 边界,炸了折 `network_failed`。
- 取消贯通:hook 管 Lua VM、transport 回调管阻塞 C 边界,同一枚旗。23 案 159 断言:生命周期六案全落(§13.4 对号),同 state 串行/不同插件并行各有计时证据。FAKE_ 原文只活在宿主侧最终头表。

## [v0.26.124] - 2026-08-30

- **manifest-backed Lua 真挂载了(Lua HTTP 单阶段 4)。** `ManifestLuaRuntime` owner:standalone 扫描与 Package 成品接管同一条路;`/plugin trust` 亮 v2 材料——Lua entry 相对路径、精确网络目的地、Secret 逻辑名与 env 名(只亮名字)、资源帽、完整指纹;权限一改旧信任即失效。
- **Package 第四类 code 组件进事务**:embedded-lua 与 process 探针同构——顶层零副作用加载、handler 对账、配齐 SecretResolver(数据目录 .env)与受控 transport;同包坏一件,Lua state 随暂存 vector 弃置即 lua_close,整包不挂。inspect/doctor 六行照契约(pure + host-http、逐目的地 DNS 安全检查,不带 Secret 发网)。
- **零执行铁证**:未信任包里放 `error("TOPLEVEL_SHOULD_NOT_RUN")` 的 chunk——零诊断即零执行。裸 .lua/v1 process/native 三条旧路一字未动。

## [v0.26.125] - 2026-08-30

- **工具确认的判断与渲染分家了(职责分离整改 1+3)。** ConfirmToolUse 只裁定与拼 ToolConfirmRequest,菜单/diff 预览/读行全搬进 `src/cli/tool_confirm_ui`;"允许并记住"的配置写入拆成 `OnToolAllowedPersist` 回调,由 UI 收到答复后调——turn_runner.cpp 不再 include config/config.hpp(双零命中)。PromptAskUser 整段 190 行同步搬 cli。事件流从此真是唯一出水口:App Server/无终端场景跑确认不再依赖终端实现。
- **wirings/ 归位纯装配根**:loop 的 PumpDueTick/FinishTick 状态机下沉 `runtime/loop_tick_driver`(打印改产 LoopTickNotice 事件,墙钟可注入);goal 的 ledger sink 搭建抽成 `runtime::goal::MakeSessionLedgerSink`(12 处 TermOut 改 Host.notify);BuildWorkflowExecutors 212 行搬进 `wirings/workflow_wiring`。四只 wiring 对照 record_session_wiring 标尺自查:无终端 IO、无业务分支外溢;boundary gate 新增 grep 门钉死。
- 顺手修一处旧注释谎言:"失源 task 落 Broken"实为 Cancelled,按实改正并单测钉住。

## [v0.26.126] - 2026-08-30

- **K2.6 可以开跨轮保留了(Kimi 单 P1)。** `ReasoningHistoryMode`(default|all) 进 RequestProfile 与 Request,`/think history default|all` 是唯一入口——不偷塞进 high/max 档。选 all 三联动:`thinking.keep="all"` 与 `thinking.type="enabled"` 同发、回传升 Always;思考关着时明报冲突拒绝,不猜。
- **三模型各有各的嘴**:K3/K2.7 由服务端固定开启,试图关 history 明说"模型不支持关闭";K2.5 选 all 当场报不支持;会话存 think_history 事件,/resume 与 /model、/provider 切换都按当前模型重校验,不把 K2.6 的 keep 状态硬带给别家。
- 回环三幕钉死:Turn 1 请求即带 keep、Turn 2 跨轮回传原字节"这轮想的要跨轮保留"、切 K3 后 keep 不出门而 reasoning 照回。金测双钉(chat_request+provider_catalog);`/think` 裸敲新增历史保留与模型能力两行。

## [v0.26.127] - 2026-08-30

- **AnySearch 参考包来了(Lua HTTP 单阶段 5)。** `examples/packages/anysearch`:manifest v2 + 208 行 anysearch.lua,四件工具(get_sub_domains/search/batch_search/extract)只用 Host API——零 Python、零 Node、零 PowerShell。SKILL 只写策略不带 CLI;网络一条精确账(https://api.anysearch.com:443 GET+POST),Secret optional。
- **Key 铁律落成代码**:服务端返回 auto_registered 时 Lua 就地摘除整字段,只回"服务端提供新 Key,当前宿主不自动保存"的非敏感提示——值不进模型、不落盘(测试钉死 FAKE 原文与字段名都搜不到)。字段名按真 API 实测校准(设计稿的 tag/params 服务端不收)。
- **假 server 13 案 196 断言**(FakeDns 注入照阶段 2 先例,manifest 原文不动):匿名无 Authorization、keyed 才有 Bearer;401/429/5xx/坏 JSON/超帽/取消各分型对号;batch 串行且取消后余笔不发。**匿名真网一笔实跑通过**(生产构造路+系统 DNS,status 200);keyed 真测按规矩 opt-in 未烧。

## [v0.26.128] - 2026-08-30

- **Lua 受控 HTTP 单收官(阶段 6)。** 六阶段一日走完:合同与 SecretResolver(0.26.119)→受控 HTTP 五道边界(122)→Lua Host API(123)→manifest 挂载(124)→AnySearch 参考包(127)→今日文档。三处文档落定:扩展指南选型表把 Lua 拆两行(裸=纯计算无 Host API/manifest v2=受控联网)、plugin-runtime 深挖校正四笔旧账("标准库全开无指令钩"三句全错)、security 新增 §9.1(Secret 来源/数据路径/五道边界/打码/cpr 备注)。
- todo §十八终稿:六阶段逐条记账,§十七条完成定义十二条全勾,§5.2 字段名按真 API 实测订正。Debug/Release 双 268/268,focused 12 册 195 案全过,docs check 47 页 605 链接全绿。
- 自此 LubanCode 有了一条完整的进程内受控联网路:Lua 描述请求,宿主握住水管与钥匙,Secret 原文六处都进不去。

## [v0.26.129] - 2026-08-30

- **多渠道 ChannelPlugin 单立了合同(阶段 0)。** 六份冻结文档落 docs/architecture/channels/:总览三条水路与六界分账(PluginTool/MCP/ChannelPlugin/Package/AppServer/Peer 各管什么)、channel.yaml schema 1(占位符信任规矩照 Package 先例)、Channel Bridge v1 帧协议(19 枚 method、JSON-RPC 五码+15 个 domain 稳定名)、入站/出站消息契约(17 态状态机、三级去重键、三档回复模式)、四层配置(项目只能收窄、八枚决策码)、安全账(威胁-防线对照、数据保留 14 类、泄露禁令)。
- 顺手修了单子的状态行事故(§19.2"状态迁移失败"原是不自动重试清单的一项,头部根本没有状态行)。阶段 1 起才动 src/channel/,照 Lua 单先例:合同先行,不接真渠道。

## [v0.26.130] - 2026-08-30

- **Compact 四分区地基落了(单阶段 0+1)。** 三缺口先堵:手工 /compact 接上反涨闸(与自动路同一只压力口径、同一只闸——拒收不换账不落事件不进 epoch);constraints/next_action schema 收紧(坏 manifest 整枚拒收,旧史不动);四份私有 turn splitter 拼成一份(`SplitIntoTurns` 进 context.hpp,空壳 user 不开轮——免得劈开工具原子组)。
- **TurnPartitionPlan 纯函数**:按 L1 工作视图 token 平衡切 N 份(整数账不引浮点、并列取早界保确定性),边界只落 turn 之间;工具原子组按 tool_use_id 收齐永远整组在一枚 turn 内(orphan/悬空点名不静默)。旧存档剥离不算 turn 不占账。
- **不调模型就能看账**:`/compact --dry-run` 出四份各有哪些 turn、各占多少 token、外置几枚 ToolResult、预计 map 几次;/context 加策略行。`compact_partition_count` 配置 2..8 可调,越界带文件路径报错。17-turn 冒烟:P1-P4 逐份列出,长 ToolResult 外置后工作视图 4120→1295 token。

## [v0.26.131] - 2026-08-31

- **自进化学会起草新工具了(阶段 6)。** 第三档判据:簇内 ≥2 场独立任务同求同一件不存在的工具名(registry.unknown_tool 实报为号),且全簇无人成功用过——才起草 process Plugin 草稿;单场偶发、撞现有工具名、无信号三种照旧 Skill-only,理由逐条给人话。草稿含 plugin.json(manifest v1+env 名+timeout)、脚手架 runner(协议铁律同 examples)、requirements 清单、逐项权限差异进演化账。
- **四类安全夹具在静态门**:恶意脚本(rm -rf/下载器/反弹 shell,注释里出现也拦)、依赖投毒(git+/http://直链/--trusted-host)、路径逃逸(`..` 与 ${plugin_dir}/.. 全包扫)、网络越权(代码带网络原语而清单未许)——发现即 error,草稿降档回 Skill-only。零进程铁证:评测计划 acceptance 白名单钉死无 command、偷运进 dev 层信任账空着挂载事务压根不接手、六用例前后 python 进程数不变。
- 契约从"code-bearing 候选明拒"改为"propose 产草稿永不启用,approve 仍拒自动晋升指路 /package trust 人工线"——安全账从"不生成"变为"生成即受四类扫描+零执行"。MCP 草稿判据已定未落,留下单。

## [v0.26.132] - 2026-08-31

- **骨架整改三桩收口(问题 4+6+7)。** settings_commands 直改内存的四处全走语义化 setter(SetProviderAuthInline/Env/None,互斥由 auth 档一锤定音、校验封进 setter),命令层字段赋值 grep 清零;worktree.cpp 归位 src/runtime/(boundary gate 白名单删一条,21 处 include 同步);config.cpp 拆出 command_permission 与 settings_local 两模块(3813→3597 行,只剩解析/merge/持久化)。
- 有意留的两笔:worktree 的 namespace 沿用 lubancode::cli(改 runtime 要动 22 文件 150 处,超纯搬家幅度,头注释写明);command_permission 不并入 command_safety(一边按用户手写前缀名单、一边按命令内容静态分析,输入语义都不同)。
- 既有行为零变:config 219/219、worktree 15/15、turn_runtime 15/15 全绿;/provider 三连切 auth 逐屏对照旧语义严丝合缝(inline 不清 key_env、env 不清 api_key)。

## [v0.26.133] - 2026-08-31

- **Token 账本与 Prompt 审计立了合同(单 A0)。** 五份 schema 冻结(UsageSample/PromptManifest/Finding/SessionSummary/Report,ToJson/FromJsonStrict 双向、未知键拒绝);UsageReport 补显式 reported 位(四家 wire 只在帧里真有 usage 才置——unknown 不再冒充 0)、request_id 迁 provider_response_id;Trajectory v2 增 model.usage.recorded 一类事件(一 attempt 一 owner,verify 拒混版本流)。
- **十二场合成夹具**:单请求全报/三步工具 cache 递增/失败重试两笔分开/主+双子代理三流分账/workflow 重试/compact map+reduce/v1 legacy/验证失败后修好/用户纠正 partial/active+truncated+corrupt。双目录重建三份 JSON 逐文件字节一致——不接真实 runtime 也能稳定出账。
- **钱算得干净**:价格表 v1 拆段乘法全程 int64 零浮点(整数 micros),reasoning 不双计;隐私 allowlist 八类 secret 扫描+九种 canary 过脱敏后报告里捞不着。A1 起的真接线压在 Trajectory P0-2 之后,照单子次序不越。

## [v0.26.134] - 2026-08-31

- **用户能在浏览器里抢回自己的手了(多前端阶段 B)。** `browser/pause|resume` 立内核手闸:暂停期间 Agent 动作受理不执行(终态 browser.paused)、用户动作照走;手闸只归用户连接,agent 连接按不动。owner 由内核按连接身份盖章(principal)——**外壳报什么不算数**:agent 连接谎报 owner:"user" 一律 owner_denied 明拒。
- **用户让路**:工作线程挑队先挑用户动作,同类 FIFO——用户不排在任何在队 Agent 动作后头;用户动作执行后递 userEpoch,Agent 拿旧快照的 ref 再动作即 stale_ref(阶段 2 机制复用)。WS 独立客户端仲裁三案+伪造案真打 50/50 全过。
- **顺手挖出 thread/start 同秒撞名暗雷**:秒级时间戳+固定 slug 同秒两场撞成同一 id,旧场被顶掉、审批账跟着悬死。修法撞了追加序号,查重与入账同锁;回归案钉死同秒三连开。

另:P0-1b SessionManager 落地(clear 八步换账、生命周期状态机、session 独占锁 PID+起始 token 身份核、崩溃恢复八册——崩在任一半路都能续办不加重复事件)。

## [v0.26.135] - 2026-08-31

- **自进化闭环整单收官(MCP 草稿+阶段 7)。** 缺口选路定死:同求恰一件不存在工具照旧 process Plugin 路径,≥2 件出 MCP server 草稿(缺一项服务封一只 server 合账)——草稿带诚实"未实现"脚手架,四类安全夹具对 mcp/ 组件全拦,零进程零挂载,approve 明拒指路 trust。
- **有限自动建议五门全落**:缺省关(坏账当关)、只提示不自动(开着时 status 后亮一行指路 propose)、门槛数值面可 inspect、拒绝后同指纹不再提示(接两本既有去重账)、命中率与接受率入账。五门各案+冒烟:关→开→提示→接受→拒绝→不再提示,接受率 100% 出账。
- **CI 五红+SIGPIPE 齐治**:双 ${package_dir} 拼接越界(词法前缀裁剪漏网,补相对结果含包根文本即拒)、macOS 无 /proc 的进程 token(sysctl KERN_PROC_PID)、让路测试 REQUIRE-abort 拆栈雷(改 CHECK+早退)、计时帽两处放宽;WS 断线后 send 在 POSIX 默认递 SIGPIPE 打死测试进程——Linux 加 MSG_NOSIGNAL、macOS 设 SO_NOSIGPIPE。

## [v0.26.136] - 2026-08-31

- **Compact 四分区双账整单收官(阶段 2-5)。** 真压缩落地:前三分区各发一次 map(严格 JSON、Markdown/缺键全拒、turn 与事件范围宿主钉死模型盖不过)、分区超预算沿 turn 边界递归拆、单 turn 超预算门口拒收零请求、map 任一块挂整次失败旧史不动。final reduce 产出 UserContract+WorkState 双账:每条要求带来源 turn、后用户覆盖前用户进 superseded(覆盖图无环校验)、assistant/tool 永不可成为用户要求来源、四类条目各带证据引用、活动待办逐字守恒。
- **压缩后接力**:双账 JSON 单围栏并进热区首条消息,下一次压缩剥旧档接力(map 请求里搜不到旧档正文);v1/旧 flat/新双账三种事件混档回放全认,29 条全量流水一条不少。三路调用方(手工/自动/子代理检查点)全切换。
- **论文式评测 300 场**(忠实模型假后端,30 题×10 抖动):token 平均省 63.9%(P90 70.5%),预算内 map 恒 3 次;约束保真 300/300、纠正归位 300/300;坏模型三型(漏抄/伪造来源/丢待办)全被校验逮住。真机冒烟 11/11:首压 3911→1513、二压接力 1513→869。真模型语义质量未量,不写"已解决多轮失败"。

## [v0.26.137] - 2026-08-31

- **骨架整改整单收官(问题 2+5)。** interactive_session_wiring 改名 assembly(不再与 wirings/ 撞车),标题账/整轮 usage 账/状态面板拼装/非 turn 通知各拆独立小类或函数(各有单测);console_input 拆三机(composer 2066 行/footer 1101/turn listener 738,共享底层留主文件),19 枚 Slot 单例归类——独占随机器搬、跨机九枚收显式共享上下文段;ChoiceMenu 评估结论"不合"(输入面与键语义已分叉,代价写明)。类头注释补齐剩余职责清单,与实际逐条对账。
- 行为零变:283 册全绿含 boundary gate/刮屏对照(与基线逐条相同的 FAIL 系假服务时序固有,无一条归因拆分);管道冒烟帮助面板/多行粘贴/菜单路全过;新文件全住 lubancode_app 不破 CMake 边界。问题 1-7 全落,单子收官。

## [v0.26.138] - 2026-08-31

- **轨迹工程通了运行时单写口(P0-2,flag 门控)。** 配置 `features.trajectory` 默认关、环境变量 LUBANCODE_TRAJECTORY 可救急——开的 session 只写 Trajectory 不碰旧 SessionStore(禁 dual-write),关的 session 一处分支不进、零字节产出。四道边界全接:AgentLoop 的 LoopBoundaryRecorder(prepared 落不住不发模型、output 落不住不执行工具)、ToolTraceHub 三栅栏(Scheduled/Started/Finished→四态终态,result 从批次尾翻译)、SessionRuntime 持 TrajectorySessionLedger、app-server thread/start 同口。
- **subagent 独立 JSONL 成真**:派工线程 SpawnSubagent 开 subagents/&lt;agent_run_id&gt;.jsonl,子 loop 全落子账,父账只有 relations.child_run_id 与 terminal hash 引用——单测钉死 main 不含子账正文。persisted_count_ 轮末补抄整路停用;TrajectoryCommandExecutor 包住 slash 分派(requested 先 durable,command_name/effect_class 落账);/record 改选段器(轨迹档零旁听)、/compact 落 typed 状态机。
- 状态机补丁:tool.execution.cancelled 不再要求 started(取消可发生在闸前)。flag 开烟测:main.jsonl 16 事件逐位齐、hash chain 过、/exit 落 command 事件;flag 关烟测:home 下无 trajectories 目录。P0-3(replay/resume)起排队。

## [v0.26.139] - 2026-08-31

- **vLLM 本地模型四 wire 修正(P0+P1)。** responses parser 补 `response.reasoning_text.delta` 分支——vLLM 0.27.x 的思考从这走,此前整段静默丢(先红后绿实证:撤掉分支夹具红 3/8,恢复绿);两只真机实录夹具进 wire replay 体系(88 帧思考流 + chat 面工具增量含 chatcmpl-tool- 前缀首帧形状)。
- **关思考方言落线**:新形状 `chat_template_kwargs_enable_thinking`(嵌套键,vLLM 唯一真生效路——顶层 enable_thinking 实测被无视);目录双预设 `vllm`(chat 面,base_url 到 /v1)+ `vllm-anthropic`(messages 面,base_url 到根,signature_required 真,"关不掉"账面如实判 ServerFixed);为本地端放行回环 http base_url(假回环域名与普通 http 仍拒,六桩单测钉死)。
- providers 手册新增「本地端点」小节,chat/anthropic 两份手写配置样例全文(报告样例的 context_window_tokens 错键已核正为 context_window)。真机冒烟:responses 面 88 帧思考解析、关思考后 reasoning 落 null 正文直出。

## [v0.26.140] - 2026-08-31

- **Compact 整单收官(§十三真机验收十一案全过)。** 假后端走真压缩生产路(分区→3×map→reduce→双账校验→换账→落盘),零产品 bug;两案首红皆为场景构造(轮次太小反涨闸拒收——恰是阶段 0 反涨闸的真机印证;压缩窗口未配则明说"未核"不装)。接力案验实:resume 后二次压缩带"上一轮总账"头、旧档不进 map、epoch 计账吻合。Debug/Release 双 287/287 全绿。
- **AGENTS.md 整单收官(P1+P2)。** `/instructions` 与 `/doctor instructions`(52 案钉子),逐 source 账带 sha/大小/离目标最近箭头;十型分账诊断(坏 UTF-8/同层遮蔽/迁移提示各归各);active write chain 超帽 fail closed——不注入半截、不发指纹、重试仍拒(与握手的根本区别);缓存 path+size+mtime 快筛、内容摘要落锤、外部编辑惰性发现。全局 ~/.lubancode/AGENTS.md 落地(身份/怎么说/怎么干活三层分工,预算最先挤掉)。
- `project_doc_fallback_filenames` 可配回退名单(默认空);发现 CLAUDE.md/GEMINI.md 只提示不读。

## [v0.26.141] - 2026-08-31

- **轨迹 Replay 与 resume 成真(P0-3)。** ReplayState 纯读折叠(对话投影带来源事件 id 与 origin、宿主注入不冒充 user、工具六事件台账、控制/证据账);state hash 确定性三处钉死(同账折两次同值、checkpoint 续折与整折同值、harness 桩与 exact 折叠同源)。exact 与 harness 两档全落——RecordedModelBackend 指纹比对不合即 divergence 不向后硬找,`trajectory replay|harness-replay|verify` 三命令进 CLI。
- **resume 七步逐字落**:run.started(start_reason=resume)→resume.source.attached(qualified refs)→跨 session command.completed;新 session id/seq 全新,source Journal 字节数不变永不 reopen append;已完成 child 不内联。拒路齐(活锁/链断/无场),悬空工具三道账分档、unknown 副作用明标不可重跑。`--continue` 与 `/resume` 两口同规;/clear 轨迹档接八步换账(P0-2 遗留顺接);/exit 退出即封口(否则 --continue 永远找不着可恢复场)。
- verifier 父子边五条交叉核全落(孤儿明报、同一 child 至多接受一次);顺手修 P0-2 真 bug:SpawnSubagent 造了 parent_run_id 却没落进 run.started, owner 边从未进账。flag 关零红,293/293 全绿。

## [v0.26.142] - 2026-08-31

- **后台子代理能再派工了(递归派工单 P0 五批)。** 病灶拆除:转发壳直指主会话 AgentTool 的旧路退役,立 AgentTaskCoordinator(台账/治理/线程唯一 owner)+AgentDispatchHandle(weak 身份+冻结环境)+TLS 执行身份(防表串层);台账一笔写事务"验父→判限额→分 id→落边", children 改派生索引不养第二本账;SubagentScheduler 退纯 admission policy。
- **结果归父不串线**:DrainCompletionNotices 只提 MainTurnContext 的根任务;嵌套孩子终态进直接父 mailbox(带"外来资料"声明,父亡未送达不 reparent);WaitingChildren+条件变量等待,等待不烧 token;取消级联整树(孩子记 ParentCancelled)。
- **结构化任务合同**:agent schema 增 task 对象(goal/deliverable 必填、分栏限额、错误带 JSON path),legacy prompt 逐字节兼容;面板详情按栏摆任务合同+lineage 行。后台→前台、后台→后台两路假后端全链真跑通(真线程真引擎),main 只收根。288/288 全绿。

## [v0.26.143] - 2026-08-31

- **vLLM 四 wire 单收官(P2+夹具补遗)。** docs/features/providers/vllm.md 手册落册并挂 fixture hash 对账(手册字节一动即红,四只 qwen3.8 夹具全数指向它);/metrics 认两代名(v0 prefix_cache_* 与 v1 gpu_/cpu_ 双前缀,旧名在场以总数为准),doctor cache 读数多一句负载语境;responses 非流式展开——output 按数组位置编号(vLLM 非流式条目没有 output_index)、reasoning 认 reasoning_text 与官方 summary 两形状,2xx 无 SSE 终止帧时走非流式回退不再误报"流意外结束"。
- **三案 wire_replay 回环**:responses 两轮(function_call 与 output 成对、思考项不回传)、messages 两轮(thinking 假签逐字节原样回传)、非 SSE JSON 裸体回退端到端。顺手修好 main 上 check_docs 旧账(README 指着从未进库的 LICENSE——按徽章声明补全文;清仓死链改指 git 历史)。294/294 全绿。

## [v0.26.65] - 2026-08-27

- **九只刮屏验收器全绿。** 终端、流式页脚、忙碌页签、视口、子代理面板等九只驱动器从烂账重钉到全过;视口驱动从 18 挂修到零,顺带揪出假服务三病(JSON 不转义、缺 Content-Length、分账锚咬错)。
- **渲染边界钉死 22 案。** 长词硬折不丢字、CJK/emoji 不切半个宽字、20 列窄窗守界、150 条工具条目、深嵌套 markdown、坏字节不崩、50 轮帧差不累计;修掉状态行窄窗越界一只真 bug。
- 扫荡记账在案:忙时打断提示可发现性、改宽瞬间流式疑停摆、子代理工具缺 title 会重试拖慢等,后续跟进。

## [v0.26.63] - 2026-08-27

- **会话总控瘦身成纯控制器。** `interactive_session` 从 6730 行收到 1114 行:组合根外迁、goal/loop/plan/peer/录制各成接线器、47 条斜杠命令改注册表分派;行为逐一照旧,四幕端到端验收全绿。
- **回合入口合一。** 用户回合与 peer 回合收成一条带来源的通道,两路差异逐一保真;peer 会话与主会话从此走同一副鞍辔。
- **会话层窄门在实现文件也成立。** 三家 wire client、安装向导、升级检查等八枚依赖出会话层,依赖方向可验。

## [v0.26.62] - 2026-08-26

- **查看态回流不再挪锚点、丢正文。** 静默回流轮重进输入时补上跨拍帧账,原处认账、画面一个像素不动;收口正文不再被整段抹掉。
- **Windows 终端缓冲收拢螺旋修净。** 视口平移改"光标带窗",不再触发 conhost 把缓冲高一路收拢、滚屏正文整段丢失的毛病。
- **端到端验收器四幕全绿。** 子代理面板刮屏驱动从 29 条过时断言重钉复活,顺手立成可复跑基线。

## [v0.26.61] - 2026-08-26

- **查看态任务完成那一刻不再崩溃。** 空坞挂提示的越界写收口(此前退场一拍必崩,七连发全复现);空坞契约补单测钉死。
- 完成通知行保持一行摘要。结论原文照旧进任务台账与查看帧,断言口径随定。

## [v0.26.60] - 2026-08-26

- **后台代理并发孵化不再被押死。** 收旧线程柄的错位 join 拆除——此前第二只后台代理要等第一只干完整轮才起步,如今按任务号对账,并发是真并发。
- 线程句柄卫生保留,误伤归零;第三幕并发前提在验收器里成立。

## [v0.26.59] - 2026-08-26

- **子代理面板验收器复活(内部加固)。** 29 条断言按现渲染形状重钉:可视区计数口径、双重编码烂码修字节、退场新规矩;一二幕稳定全绿。
- 重钉过程逮出三处真病灶(错位 join、空坞越界、回流锚点),随后三版逐一修净。

## [v0.26.58] - 2026-08-26

- **事件流成为唯一出水口。** 25 字段回调结构退役,终端画面、JSON 协议、子代理台账、workflow 节点全吃同一份事件流;画面与协议逐字节照旧。
- **散落终端输出归口。** 命令层与 CLI 层 16 个文件约 760 处直写清零,统一走输出端口。
- 事件账面加法:子代理条目独立可溯,回合多出 step/批次边界事件。

## [v0.26.57] - 2026-08-26

- **三份外壳的回放归一。** loop、goal、workflow 各自的事件账本接入同一份回放接口:坏行跳过不废整场,信封、次序、恢复入口只此一份;三家恢复测试全绿。
- workflow 的 agent 节点改走统一回合引擎,收场分型与事件口径与其余各路一致。
- 后台采样统一走同一只钟,时间戳口径全仓一致。

## [v0.26.56] - 2026-08-26

- **大会话开始开刀。** 终端输出立端口,354 处散打归口;transcript 查看态控制器、命令展示层逐一拆出,/model、/trace、/memory 等肥案外迁。
- `interactive_session` 从 6730 行收到 3993 行;子代理面板从静态槽改实例注入,手工投影清零。
- 斜杠命令、面板交互、静默回流行为逐一照旧。

## [v0.26.55] - 2026-08-26

- **Agent 自立门户。** 上下文管理分家(ContextManager),model 只存一份,十二个 setter 收敛成正门一只;`/model`、`/soul` 的改动从此如实计入前缀账。
- 五层请求改写后端整体退役,改走皮上档案管道;真实缓存命中首次出现在统计行。
- 会话/peer/技能三个域的模块补齐各自命名空间。

## [v0.26.54] - 2026-08-26

- **子代理提示词补齐四段。** web、mcp、lsp、平台段的开关写进代理档案,子代理默认与主代理同段,能力不再天生缺角。
- **主会话与子代理共用同一副回合鞍辔。** 续投外环、取消链、收场分型、Stop 续跑各留一份;九百行的子代理执行体按调度、台账、隔离三件拆分。
- 子代理事件流并入宿主账本,工具起止与用量可溯。

## [v0.26.53] - 2026-08-26

- **三份外壳的横切设施归一。** 预算闸(两口径)、退避(三档)、id 发号、统一时钟各一份;loop、goal、workflow 换用后对外节奏、停因文案不变。
- 台账 id 同源:loop-N、goal-N、run 事件序号出自同一发号局,恢复后续号不重号。
- 连败自停、防空转等连撞计数收进公共件,各域字段照旧落档。

## [v0.26.52] - 2026-08-26

- **src/agent/ 只剩引擎。** 会话域、peer 域、技能域九件外迁各自门户(sessions/peers/skills),61 行引用改净;纯搬家,行为零变。
- 会话存档读写与列表在搬迁后真机验活。

## [v0.26.51] - 2026-08-26

- **事件流升正房。** 六处装配点切到事件流配置,适配器成唯一出水口;终端先落账再画屏,屏上一个字节不变。
- app-server 手拼事件整块拆掉,改同一适配器;六行事件次序与冻结协议逐一对上。
- "思考中"转轮挪出传输层,UI 件不再混进请求栈。

## [v0.26.50] - 2026-08-26

- **堵住两条绕开正门的暗道。** workflow 工具节点改走统一工具链:钩子、权限确认、trace 落账、schema 复检全数生效;同仓不再存在一条全监管、一条裸奔的两条工具执行路。
- **六处单发采样收敛一个原语。** 压缩、标题、记忆抽取、goal 评估、workflow Llm 节点统一走小模型活服务,用量口径一致、错误兜底同规。
- workflow 工具节点过账有七案测试钉死。

## [v0.26.49] - 2026-08-26

- **四家 API 传输合流。** cpr 骨架、状态码提取、错误分类、请求兜底归一份公共传输层,四家 client 从 1117 行收到 524 行。
- wire 请求逐字节不变(既有单测钉死),gemini 的鉴权头特例保留;网络错误的分类与文案口径四家一致。

## [v0.26.48] - 2026-08-26

- **模型目录升 v2。** providers 目录重排,模型条目绑定所属 provider,推理档位走规范字段;目录数据大刷新,`/model` 列表出新。
- **请求档案立户。** model、推理档位、推理能力只此一份出处,`/model`、`/think` 切换即时生效如旧;档位不再借 extra_body 偷渡。
- 修复上一版断线遗留:loop 命令两处半笔改回正名。

## [v0.26.47] - 2026-08-25

- **POSIX 终端重画与忙时输入走稳了。** Linux、macOS 的终端恢复逐帧重画，模型工作时仍能键入并排队下一条；输入态、队列与状态栏不再互相挤占。
- **README 追上如今的用法。** 中英两页同步到当前 Provider 目录、三档角色模型、按需 artifact 摘要与可跳过的初次启动；过期的体积、内存对比一并撤下。
- **使用说明添了真机画面。** Release 二进制接本地测试 Provider 实机截取子代理队列与 Markdown 渲染，发行包里的文档打开便能看见，不带真密钥。

## [v0.26.46] - 2026-08-25

- **初次启动不再强填连接。** 开局可选“添加 Provider”，也可暂时跳过，先进入主界面；未连接时仍能使用 `/provider` 与 `/provider add`，配好后当前会话立即接上。
- **Provider 向导把去路摆明。** 汇总页明写回车保存、编号修改、`n` 放弃；目录预设的编号如今真能跳回改单项，重复菜单、重复按键提示与目录超时后的本地回落一并修顺。
- **终端几处错位收拾干净。** Composer 高度与光标同源计算，长时间“思考中”不再叠画；自动 `edit_file` 预览不再留白，模型切换与 UTF-8 标题也走稳同一条路。

## [v0.26.45] - 2026-08-25

- **坏 UTF-8 不再炸穿整轮请求。** 消息、工具结果与标题上 wire 前统一清洗，截断一律贴着码点边界下刀，`incomplete UTF-8 string` 这类 316 异常有了总闸。
- **微压缩退到按需后台。** 缺省不再改写旧消息；需要摘要时才生成，作为尾部新消息追加，原文与 artifact 仍可追回，输入框也不再等它收工。
- **Composer 与模型配置各归一本账。** 空闲、忙碌、重放共用同一套输入布局；`/model` 按 Provider 记住选择，三档角色模型可直设，缓存命中与上下文占用也分口径展示。

## [v0.26.44] - 2026-08-25

- **三档角色模型一条命令直设。** `/model <role> <id>` 直接指定主/子代理/compact 各档位的模型，不用再翻配置；provider 向导的搜索与确认输入、切换面板原地重画一并修顺。
- **空壳工具 schema 不再拖垮整轮请求。** 工具定义上 wire 前先兑正，非法/不完整的空壳 schema 提前拦下，整轮请求不再因此失败。
- **文件与文本工具的手感修了一轮。** `search` 的 path 直接认单个文件；`read_file` 显示分页参数与真实行数；多处 UTF-8 字节截断边界修复，多行光标与输入框高度也顺手修正。

## [v0.26.43] - 2026-08-24

- **选择菜单会自己进搜索了。** 候选超阈值自动进搜索分页模式，长列表不再一屏装不下；resume 选择器原地重画，不闪不跳。
- **模型目录扩到 75 家。** 目录扩充并修了 embed 分块；provider 连线规范化四名，新增 google-generate-content。
- **子代理与 provider 工作流修顺。** 子代理/provider 工作流修复、回合活动与底栏间距恢复、未命名待执行工具不再占位、自动生成的会话标题正确渲染；启动警告改走 stderr，不污染协议流。

## [v0.26.42] - 2026-08-24

- **终端转录有了信息层级。** 用户输入铺整行淡底色块(不只染字,多行连续色面,CJK 折行不切宽字),live、/resume、Ctrl+L 三路同一副 formatter;块与块之间按间距表留气口,消息收尾不再贴脸。
- **轮与轮之间一道淡色横线。** 重画回放从第二轮起紧贴用户块画分隔线,与既有分界线同一根线,不双打不累计;纯文本/重定向模式全退化零 ANSI。
- **run_command 输出帽不再切半个汉字**(0.26.41 后补的平台修复随车):四处读取刀口对齐 UTF-8 码点,goal 线落档一律过清洗兜底,316「未预料的异常」绝根。

## [v0.26.41] - 2026-08-23

- **goal 真的会自己跑了。** 回合收口自动采证(工具账里只取 hash 与事实,不抄正文)、过 checkpoint、走独立 evaluator 判定,continue 才续跑下一迭代;子代理的完成与用量回流进 goal 的账,fork 记 lineage 不复活;goal/loop 的十六枚 typed 命令挂上 app-server,终端与远端同一份执行体。
- **后台自动活儿看得见了。** 状态栏恒亮「goal run·iter3·r2 · loop×2 next 4m」;goal 六枚生命周期事件进 hook 分发(只许看不许改判定);loop 事件接 EventSink、审批等待接 broker、ESC 空闲态一键停全部活循环;/loop list 的预览不再切半个汉字。
- **/plan 的审阅远端也能答了。** plan.review 挂上协议面,三对 id+revision+sha 匹配才落账,批准回执带执行档;迟到的答复报失效不冒充。

## [v0.26.40] - 2026-08-23

- **/loop 定时循环立起来了(features.loop 门,默认关)。** 到点自动起一轮、single-flight 不叠拍、失败退避、连败熔断;错过的时间窗合并记账不补跑成灾,崩溃各点恢复后默认暂停等人发话;Ctrl+R 历史里不混 scheduled 消息。
- **与 goal 同一口泵吃饭。** 五档优先级调度(用户排队与待交互最高,goal 续跑与 loop 拍同档,维护最低),公平计数「连跑三轮让一拍」;timer 只发唤醒,泵位在用户排队与 peer/子代理回流之后,谁也别想霸场。
- **typed 合同与持久账齐了。** loop 七命令七事件进 Runtime 合同,loop_control 窄工具只认本拍任务;全生命周期落 session 档、老档兼容,goal 的续跑请求经同一只调度泵分流。

## [v0.26.39] - 2026-08-23

- **/goal 持久目标落了地基(默认关,配置里 [features] goals = true 才露)。** 一只 active goal 的状态机与 revision CAS、七动作 slash 面、goal 事件行五种进会话档;resume 整场重建,feature 关落 SuspendedByPolicy,终态不复活。
- **预算与防空转是硬闸。** elapsed/迭代数/token 三道预算(usage 未报告不冒充)、无进展三轮自动暂停、同一堵点三轮转 Blocked、provider 连败另有闸;goal 的判定证据走独立 evaluator 请求,achieved 要准则全过+新鲜证据+remaining 清空,差一样都不算成。
- **compact 带着目标走。** compact_v2 事件带 goal 快照与守恒 hash,resume 逐项对账;与 loop 的分流合同口已立(五档优先级、公平计数「连跑三轮让一拍」),timer 与泵归 loop 单接线。

## [v0.26.38] - 2026-08-23

- **命令进程有了一条生命线。** 起没起、活着没、怎么退的,各自立账:退出码不再丢也不再借 0(unknown 如实说),Stop 先杀整棵进程树再盖章,后台注册的先起后表的竞态与并发环境串值一并修死;原生句柄台账 + 每任务 Job,POSIX 走唯一收尸线程。
- **目录与引号的老坑填了。** cwd 全程走操作系统参数(Windows lpCurrentDirectory / POSIX chdir),拼接 cd 那条路整个删除;POSIX 单引号转义修正,PowerShell 退出码包装重写(真机 5.1 十案验证);输出超限算失败、后台日志 0600 防泄密、8MB 截断、终态保留 200 条,坏编码出关先清洗。
- **shell 明牌,bash/pwsh 装了才认。** /doctor 探路径/版本/login/TTY 一眼见底;shell=bash 或 pwsh 进了 schema——本机没装就明说,不偷换默认;/doctor 与文档同步。

## [v0.26.37] - 2026-08-23

- **有了 Plan 模式这道硬闸。** /plan 进入只读研究档:能看能搜能问,写文件、跑改动命令一律在工具执行前被拦(probe 类只读探针放行);计划落进会话档,/plan status 随时看,退出即回 Default。--mode plan 启动链与子代理、PTC 同过一道闸。
- **计划先审后干。** 模型交出 proposed_plan 后回合收口扫一遍(防代码块、嵌套、半截),弹四选审阅框:批准即切换执行档并附执行简报接着干,拒绝则计划打回重拟;审批三对 id+revision+sha 匹配,resume 恢复不重复落账。
- **模式状态全程可观测。** mode/plan 事件行进会话 JSONL,Runtime 合同立 mode.set/plan.review typed 命令与 mode.changed 事件,Web/Tauri/app-server 吃同一碗;模式模板两份不可覆盖,项目目录的同名文件无效。

## [v0.26.36] - 2026-08-23

- **每枚工具调用都有了一本生命周期账。** 何时收到、越过权限闸、踏进副作用、拿到结果、写回历史,五道栅栏逐枚落进会话档;崩溃后重启按账判定「没跑/跑完/可恢复/结局未知」,能重放的才重放,/trace 五档查询连重启前的账都翻得出。
- **改错的文件能撤了。** write_file/edit_file 自动留 preimage 凭据,新工具 undo_file_edit 走正常确认门做条件式撤销——当前内容对得上才动手,被改过就给三方对照拒绝;补偿意图(谁想撤谁)无论成败都记账,审计有迹可循。
- **多方吃同一本账。** MCP 换传输层的代数入账、jsonrpc id 成一等字段;子代理的内层工具事件经只读汇并轨,带父子边不交错写盘;app-server 新增 trace/query 断线补账与脱敏导出,归档/删除连 context 仓一起搬。

## [v0.26.35] - 2026-08-23

- **一轮回答收成一册账。** 回合结尾落一道带字的分界线 `──── Worked for 6m 41s ────`，打断写 `Stopped after`、失败写 `Failed after`；错误与预算耗尽不再从中途裸退漏掉尾线，管道模式也落纯文案版（时间账属于 automation 契约）。
- **工具看得出批次了。** 同一次模型响应吐的多枚工具先整批登记"本拍排队中"，再逐枚点亮执行；ESC 后跑完的照旧、跑着的记打断、没轮到的如实标"未执行"，屏上不缺枚。五行 PowerShell 只画一个条目，标题带 `+4 lines` 不再横铺。
- **Working 计的是整轮了。** 活动条认 turn 不认单次请求：正文流、工具批次、下一次模型请求都不熄、秒数不归零；ESC 后换"正在停…"，终态落账才退场。Ctrl+E/Esc 返回不再追打横幅，token 长行退到详细态（Ctrl+O），Ctrl+L 重放与实时画面同一颗渲染器。

## [v0.26.34] - 2026-08-23

- **无界面后台接口补齐了血肉。** 工具条目带中立 diff 行表、回合用量与上下文压力实时通报、图片输入入协议;出站队列的增量合并落了真(delta 并条、溢出通报带账);jsonrpc 字段去留、事件序号、图片字段名一并冻结成文。
- **后台也能管会话了。** thread/archive、unarchive、delete 三法接通统一收口,开着的热线程拒绝动;thread/list 与终端同吃一碗结构化摘要,/workflow 的运行快照与增量事件也从同一扇门出来。
- **SSH 承载有了实跑口径。** 本机管道冒烟六项全过(握手/坏报文/协议纯度/断线退场/无孤儿);真 SSH 的手测口径与完整协议文档落 docs,三平台方法面一览无余。

## [v0.26.33] - 2026-08-23

- **Workflow 并行 map 的产物不再串位。** 并发分支的产物落位改预分配槽、各写各的下标,收拢后按 items 顺序拼装;修掉了共键覆写导致的乱序取值(libc++ 时序下现形的竞态,TSan 已清)。
- **中文名会话在 Windows 上删得掉了。** 会话搬删的档名拼装改走 UTF-8 通道,不再被 ANSI 代码页解成乱码名——此前中文档在 Windows 上删除/归档一直 NotFound,是真产品 bug;相关测试夹具一并改掉「写找同错相抵」的侥幸,柄收口回归补齐。

## [v0.26.32] - 2026-08-23

- **显示与业务分家(地基)。** 引擎按 engine/runtime/terminal 拆开编译：工具改动的行级 diff 有了中立行表，Web 与终端吃同一份数据；事件流合同落地，终端与 JSON 两只事件出口同流同账，后续 Web/Tauri 接同一颗运行时，不再复制终端逻辑。
- **会话内核可脱离终端单测。** 会话存档、权限与发号搬进 SessionRuntime；`/model`、`/resume` 与审批回答有了类型化接口，远程前端不再伪造 slash 字符串；交互会话类更名终端控制器，只管画面接线。
- **引擎日志不再裸写标准流。** 模型端与钩子的诊断改投统一日志出口，app-server 的 stdout 从此只剩协议字节；依赖边界进了测试账，引擎层混入终端件直接编不过测试。

## [v0.26.31] - 2026-08-23

- **本地插件一站齐活。** lubancode plugin init python 一键生成插件三件套;Lua 插件进统一台账并加三道软墙(pure 档缺省关 io/os/package、指令预算防死循环、内存帽);原生插件三平台一个形状(Windows/Linux/macOS 真编真载),ABI 升 v2(版本协商/shutdown/宿主分配器)且兼容读 v1。
- **插件跑在最小环境里。** 进程插件整环境替换:只递 PATH 与声明的几个变量,宿主的密钥一概到不了插件;项目级 .lubancode/plugins/ 要过内容指纹信任门,改一字节就得重新批;/plugin inspect|doctor|test 一套管理面看得见状态。
- **常驻进程有据不做。** 冷启动实测 Python 全协议往返 14~50ms,占整次调用 1%~4%——重依赖该修插件设计,不是宿主先造握手心跳,数字与依据落进架构文档,哪天变了再立单。

## [v0.26.30] - 2026-08-23

- **办事章法能存成一张图了。** 新增 Workflow:把一套流程说给 LubanCode,追问缺口、生成有类型的执行图(先校验、预览、确认再装进 .lubancode/workflows/),往后 /<别名> <参数> 照图开工;图有 ASCII 与 Mermaid 两种画法,项目级遮用户级、撞名别名禁直呼。
- **引擎一条龙:并行汇合、重试回落、断点恢复。** 五种 join 策略(all/all_settled/any/quorum/race)、map/reduce、取消与步数·时限·token 三道预算;审批与补问走 Broker 悬起,journal 落盘带脱敏,断点续跑已成功的节点不重跑;副作用节点没幂等键不许重试,secret 明文直接拒载。
- **动态有缰绳。** 规划器只许用图里标注 template: 的积木补图,补丁 append-only、悬边越权全拒、动到既有节点或新增交互要再问一道;运行态出结构化快照与增量事件,终端与将来的 Web/Tauri/app-server 同吃一碗饭。

## [v0.26.29] - 2026-08-23

- **会话台账三副眼镜。** /resume 台账里 Ctrl+T 看整场转录(大文件只取头尾,收起回原行)、Ctrl+E 摊开标题/目录/id/模型/消息数与更新时间、Ctrl+O 紧凑舒展两种画法;浏览全程不动盘。
- **归档先行,删除另开明路。** 新增 lubancode archive/unarchive/delete 顶层命令与会话内 /archive、/delete:归档字节原样搬进 sessions/archive/ 且默认列表不再打扰;永久删除要过确认屏(标题+完整 id+目录,缺省取消),标题重名列短 id 绝不猜,路径越界一律拒;回合在跑、后台子代理在忙都拒绝动手。
- **搬删只此一家。** SessionLifecycle 统一收口,终端 slash、顶层命令与 app-server 同吃一碗结构化账(thread.list 结构化摘要、archive/delete 走 typed 命令),代码里搜不到第二条私搬私删路径;Windows 开句柄先收柄再动文件的回归有测试钉着。

## [v0.26.28] - 2026-08-23

- **工具文案全量双语收官。** 剩余九件工具(网页搜索/抓取、tool_search、skill、会话列举与跨会话传话、worktree、项目记忆、PTC)的描述与参数说明全部迁入语言分档;英文会话拿到全英文工具表,缺省中文与旧版逐字节一致,`LUBANCODE_LANG` 切换即时生效。
- **文案一致性进了测试账。** 语言驱动器扩到 100 节中英双语逐字节比对;工具源码不再容得下游离中文描述,grep 检查与负例钉进回归,后续新工具照此门进。

## [v0.26.27] - 2026-08-23

- **无界面后台接口接通。** `codex app-server` 同类能力落到 LubanCode：JSON-RPC stdio、thread/turn 生命周期、流式事件、审批反向请求、取消与硬超时已经接上；终端与后台接口共用运行时合同，不再各养一套 Agent 逻辑。
- **会话与终端更像一张清楚的账。** 裸敲 `/resume` 进入可搜索、筛选、排序的全屏会话台账；排队消息可随存档恢复；工具文案按语言分档，运行条目、ID 与状态刷新也收回统一口径。
- **插件、文档与三平台一道收口。** Process Plugin v1 冻结 manifest/stdio 合同，Lua 调用与主/子代理挂载补并发和去重；官方 docs 与 Skill 同包安装，文档按功能、参考、架构、开发分层；Windows、Linux、macOS 的文件时钟与安装路径差异补齐。

## [v0.26.0] - 2026-08-15

- **终端交互大修。** 子代理面板移到输入框上方：任务全量列示可选，`x` 停止/清除，`Ctrl+X Ctrl+K` 两段确认停全部，Enter 进查看态后可直接给那只代理递话；排队消息在工具边界按序送达，`Esc` 打断并立即送，`Shift+←` 取回编辑；流式打 `/` 出补全，context 状态栏回合内实时刷新（`~` 标旧值），流式 Shift+Tab 切档，思考可 Ctrl+O 展开，ask_user 不再被子代理状态遮挡。
- **模型侧 worktree 全套。** 新增 `worktree` 工具（enter/status/list/exit），大改动住进隔离分支，写主树/命令 cwd/git 改道三道闸拦截；子代理可带 `isolation: worktree` 各住各的隔离房，干净自动清、有活留房；`/resume` 验明正身后回搬住房会话；`.worktreeinclude` 把 `.env` 一类带进新房。
- **更结实也更清爽。** 工具输出含非法 UTF-8 不再杀整场会话（清洗前移到公共信任边界，`read_file` 对编码明规矩）；后台子代理完成后结果自动回流，空闲会话不再冻死；main.cpp 从六千余行拆到 39 行，app 层分件立编译边界，改一条命令不再重编整个程序；GLM-5.3 进厂家目录。

## [v0.25.1] - 2026-08-14

- **录一遍，生成技能。** `/record` 开录后在 LubanCode 里做一遍活计，工具调用、备注、验收一路记下；停录起草 SKILL.md，预览点头后装进项目级或主目录级，本场立刻可用。入参密钥全程打码，必须明着开录，状态栏常挂 REC 标记。
- **排队输入不再拧成一股。** 输入行只画你正打的字；落队后正文挪到输入框上方逐条摆，空输入按上键取回改写，Delete 删、Esc 放回；「Esc 打断」挪进状态行。
- **同机跨会话能递话。** `/peers` 列出本机其它会话，`/send` 送一张字条；收件只挑轮次边界，不掐正在跑的工具，来信不能批工具、不能改配置，slash 只算文字。Windows 走具名管道，Linux 走 Unix socket，均限当前用户。

## [v0.25.0] - 2026-08-14

- **思考与调度看得见。** 模型的 thinking/reasoning 过程实时上屏；异步子代理带常驻面板与 Explore 模式；后台命令有任务台账、完成通知、输出查询与停止。
- **选择全用方向键。** 工具确认、配置向导、slash 补全改为方向键选择，Enter 确认；Ctrl+O 原地展开最近一条转录。
- **长输入更稳更顺。** 正文与输入框软换行；终端重画按帧差只描变动行，长会话不卡不画偏；输入法整词提交与快打连击不再丢字迟到；安装后 Windows PATH 即时生效。

## [v0.24.1] - 2026-08-06

- **模型接入更全。** 新增 Chat Completions 协议与内置 Provider 目录；可从常见厂家预设中添加服务，也能在会话里更新目录、切换 Provider 与模型。
- **项目知识能接着用。** 新增默认关闭的项目记忆，可召回稳定事实与偏好；本地 Skill 支持安装和管理，会话恢复、续聊与导出也补齐了交互细节。
- **终端操作更稳。** 状态栏可配置，彩色 diff 能按语言着色，大段粘贴、待办更新与工具条目重画更可靠；LaTeX 可铺成分式、根式、上下限与矩阵；模型列表会沿用当前项，也可按 Esc 取消；切到详细模式时，已经执行过的工具会立即展开。

## [v0.23.5] - 2026-07-24

- **项目规矩自动生效。** 新增 `/init`，可生成并载入 `AGENTS.md`；指令从 Git 根逐层合并，主代理与子代理一同遵守。
- **粘贴长文不再误触。** 多行内容会折成一枚占位，仍可前后编辑；按下 Enter 后再展开原文交给模型。
- **长回答更好读。** Markdown 改为分段增量渲染，标题、列表、表格与代码块边生成边收束，滚屏后也不易露出原始标记。

## [v0.23.4] - 2026-07-23

- **单选不用再敲编号。** `ask_user` 改为方向键菜单，用 `↑`、`↓` 移动，按 Enter 确认。
- **多选可以逐项勾选。** 按空格选中或取消，至少选中一项后再提交。
- **取消与自由填写仍在。** Esc 可随时退出，移到“自己填写”便能输入自定义答案；管道等非交互场景仍保留编号输入。

## [v0.23.3] - 2026-07-23

- **工具状态原位更新。** 黄色运行条会在完成后直接变成绿色或红色终态，不再把开始与结束各留一行。
- **输入框及时回来。** 工具条目写完便重建底栏与输入框，不必等下一段模型正文才恢复。
- **滚屏时少花屏。** 工具条目、子代理输出与底栏共用一套重画事务，长输出下的错位、重复与覆盖一并收紧。

## [v0.23.2] - 2026-07-23

- **Working 不再挡住输入框。** 思考状态与输入框同屏摆放，模型尚未吐出正文时也能继续键入。
- **工具续轮仍可排队。** 一件工具执行完、模型再次思考时，光标仍停在输入区，下一条消息照常进入队列。
- **发包链更稳。** GitHub Release 上传组件升到新版本，三平台安装包仍由标签流水线自动生成。

## [v0.23.1] - 2026-07-23

- **Provider 会记住。** `/provider switch` 成功后保存当前选择，下次启动仍走同一家服务。
- **切换后立即换屏。** Provider、模型与推理档位更新后，横幅和状态会按新配置重画，不再留下旧信息。
- **交互工具更完整。** 新增 `ask_user` 单选、多选与自由填写；执行期间常驻输入框和消息队列，彩色 diff 收起后也不再留下背景色块。

## [v0.23.0] - 2026-07-22

- **编程代理主流程齐备。** 支持读写与搜索文件、容错编辑、前后台命令、diff 确认、子代理、消息排队及 Esc 打断。
- **模型与扩展接成一体。** 接入 Anthropic Messages 与 OpenAI Responses，并提供上下文压缩、会话恢复、MCP、LSP、Skills、Lua、C ABI 插件和联网工具。
- **三平台可以直接安装。** Windows、Linux 与 macOS 均有自动构建的发行包和安装脚本，CI 分别用 MSVC、GCC 与 Clang 编译测试。

[v0.26.35]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.34...v0.26.35
[v0.26.34]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.33...v0.26.34
[v0.26.33]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.32...v0.26.33
[v0.26.32]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.31...v0.26.32
[v0.26.31]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.30...v0.26.31
[v0.26.30]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.29...v0.26.30
[v0.26.29]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.28...v0.26.29
[v0.26.28]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.27...v0.26.28
[v0.26.27]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.0...v0.26.27
[v0.26.0]: https://github.com/relic-yuexi/LubanCode/compare/v0.25.1...v0.26.0
[v0.25.1]: https://github.com/relic-yuexi/LubanCode/compare/v0.25.0...v0.25.1
[v0.25.0]: https://github.com/relic-yuexi/LubanCode/compare/v0.24.1...v0.25.0
[v0.24.1]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.5...v0.24.1
[v0.23.5]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.4...v0.23.5
[v0.23.4]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.3...v0.23.4
[v0.23.3]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.2...v0.23.3
[v0.23.2]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.1...v0.23.2
[v0.23.1]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.0...v0.23.1
[v0.23.0]: https://github.com/relic-yuexi/LubanCode/releases/tag/v0.23.0
