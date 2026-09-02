## description

把独立任务委托给子代理。先想一个 4~16 字(英文 2~6 个词)的语义短标题填 title——名词短语或短命令,能彼此区分,不要照抄 prompt 首句、不要塞路径清单或套话;再把完整的任务说明写进 prompt。title 给人看(代理面板/日志),prompt 给子代理执行,两者各司其职。agent_type=Explore 是只读代码搜索代理;general-purpose 能研究、执行多步任务和改代码。子代理有独立上下文,只把结论交回主对话。执行模式看 execution_mode(缺省 auto):交互会话里探索、生成、写代码、调研这类独立任务用缺省 auto 即可——后台独立跑,完成后结论自动交回,主对话还能继续干别的;不要为了拿结果习惯性写 foreground,后台结果一样会回流,只有紧接着的下一步非等这份结果不可才显式写 foreground。管道/单发场景 auto 等价前台(阻塞等结论)。后台任务不能弹权限确认,未预先放行的操作会被拒绝。子代理看不见当前对话历史,prompt 必须自包含。

## param.title

任务短标题,必填。给人看的语义字段:中文 4~16 字、英文 2~6 个词,名词短语或短命令,能与其他任务区分。不得照抄 prompt 首句,不得含路径清单/验收全文/换行/制表符,硬上限 40 显示列。先概括 title,再写完整 prompt。

## param.prompt

交给子代理的完整任务说明。LubanCode 每只子代理进场时都没有父会话上下文。把它当作刚走进屋子的聪明同事来交代:它没看过当前对话,不知道你试过什么,也不知道这件事为何要紧。

- 说清想办成什么,为何要办。
- 写出已经查明什么,又排除了什么。
- 补足周边事实,让它能自行判断,不只会照令行事。
- 给出必须知道的文件路径、行号、错误原文、准确命令与边界。
- 若只要短答,明说篇幅;收工要带回结论、改动、测试或风险,也要明说。
- 查东西就交出准确命令;做调查就交出真正问题,别拿一串死步骤遮住问题。

绝不要把理解也甩给子代理。别只写“根据发现修复问题”;派工说明本身应证明你已经弄懂,写清究竟要改什么。推荐按“背景—任务—报告”排布,普通自包含任务句也合法。宿主不解析 Markdown,不补造栏目,不重写正文;只校验字符串、非空、NUL 与 32 KiB 总帽。

## param.agent_type

子代理类型:Explore 只读搜索分析;general-purpose 可做多步操作(默认);或 /agents 清单里的自定义 Agent 名(各自带工具边界、预装技能与预算,清单以 /agents 实时输出为准)。

## param.execution_mode

执行模式,缺省 auto。auto:交互会话里默认后台独立跑(结论完成后自动交回主对话,主对话可继续干别的)——不要习惯性写 foreground,只有下一步非等这份结果不可才显式写;管道/单发场景 auto 等价前台(阻塞等结论)。background:立刻返回任务编号,后台独立跑;background 任务不能弹权限确认,未预先放行的操作会被拒绝。foreground:本次调用阻塞等子代理结论。旧参数 run_in_background 仍认(true=background,false=foreground);两者都给时,显式(非 auto)的 execution_mode 优先。

## param.run_in_background

(兼容旧参)是否放到会话后台运行:true 等价 execution_mode=background,false 等价 foreground。新调用建议用 execution_mode。

## param.isolation

worktree = 给子代理单独开一间 git worktree 隔离房干活:写不碰主 checkout(文件/命令/git 三道闸拦),干完没改动房自动删,有改动则保留并在结果里附房路径与分支,由主代理或用户收尾。改代码的多步任务建议带上;只读摸排不必。缺省 none。

## persona.general

你是 general-purpose 子代理,能搜索、分析并完成多步任务。专注给定任务,完成后直接给出结论,不要寒暄。

## persona.explore

你是 Explore 子代理,专门快速搜索、阅读并分析代码库。只读,不得改文件、启动会改动环境的命令或做别的写操作。完成后给出简明结论和具体文件位置,不要寒暄。

## param.template

(可选)任务书套壳:full = 宿主给 prompt 套六件套引导壳(单子路径/范围红线/环境实情/纪律/完工标准/回报格式),任务原文逐字节保留。模板只引导不强制,自包含已写清的简单任务不必套;大单子(修 todos 单、多步改码)套上更稳。

## env_appendix.header

[宿主注入·本机环境附录] 以下是宿主在会话启动时探测的本机构建环境事实,与任务正文无关;构建与测试照此办理,不必自行摸索。

## env_appendix.preset

仓库构建账:CMake preset {0};构建目录 {1}(相对仓库根);ctest 配置名 {2}。

## env_appendix.offline_deps_ready

离线省时路:本机 {0}/_deps 已备齐。换树构建时把它整目录拷到该树同名位置,configure 再加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON(依赖全走本地,网络不通也能配);全量 FetchContent configure 约十分钟,能省则省。

## env_appendix.offline_no_deps

构建树已在,但 {0}/_deps 不齐:离线路走不通,依赖走 FetchContent 全量 configure(约十分钟)。

## env_appendix.offline_no_build

本机还没起构建树:先 configure(依赖走 FetchContent,约十分钟);别处若有备齐的 _deps,整目录拷来并加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON 可走离线。

## env_appendix.ctest_windows

ctest 规矩:必带 -C {0}(多配置生成器);ctest 前把 USERPROFILE 指到 Windows 路径的临时目录,别碰真用户主目录。

## env_appendix.ctest_posix

ctest 规矩:多配置生成器必带 -C {0}。

## env_appendix.clean_first

动了头文件就带 --clean-first 重建,别吃陈旧构建产物的亏。

## template.full

[宿主套壳·任务书六件套] 下面先录派工者任务原文,再附六件套核对单。六件套是引导不是格式铁律:原文已写清的不必重抄,缺的项照提示补齐或向派工者问明,别自行脑补。

===== 任务原文 =====
{0}
===== 原文完 =====

[六件套核对单]
1. 单子路径:本任务出自哪张单/哪段需求?原文没写就先问明,别猜。
2. 范围红线:只许动哪些文件/模块?哪些明确不碰?
3. 环境实情:宿主若在任务书尾部附了 [宿主注入·本机环境附录],照附录构建测试;没附就动工前自己摸清本机构建环境,并在回报里写明。
4. 纪律:提交信息规矩、push 与否、版本号动不动、单子批次只勾真验证过的。
5. 完工标准:怎样算修完——测试全绿、回归零、验收命令过。
6. 回报格式:完工回报带分支、commit 号、测试汇总原文、落点(文件:行)、新增测试册。
