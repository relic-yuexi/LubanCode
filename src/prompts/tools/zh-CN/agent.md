## description

把独立任务委托给子代理。先想一个 4~16 字(英文 2~6 个词)的语义短标题填 title——名词短语或短命令,能彼此区分,不要照抄 prompt 首句、不要塞路径清单或套话;再把完整的任务说明写进 prompt。title 给人看(代理面板/日志),prompt 给子代理执行,两者各司其职。agent_type=Explore 是只读代码搜索代理;general-purpose 能研究、执行多步任务和改代码。子代理有独立上下文,只把结论交回主对话。执行模式看 execution_mode(缺省 auto):交互会话里探索、生成、写代码、调研这类独立任务用缺省 auto 即可——后台独立跑,完成后结论自动交回,主对话还能继续干别的;不要为了拿结果习惯性写 foreground,后台结果一样会回流,只有紧接着的下一步非等这份结果不可才显式写 foreground。管道/单发场景 auto 等价前台(阻塞等结论)。后台任务不能弹权限确认,未预先放行的操作会被拒绝。子代理看不见当前对话历史,prompt 必须自包含。

## param.title

任务短标题,必填。给人看的语义字段:中文 4~16 字、英文 2~6 个词,名词短语或短命令,能与其他任务区分。不得照抄 prompt 首句,不得含路径清单/验收全文/换行/制表符,硬上限 40 显示列。先概括 title,再写完整 prompt。

## param.prompt

交给子代理的任务描述,必须自包含——子代理看不见主对话历史,任务目标、范围、期望的输出形式都要写清楚。

## param.max_steps_per_turn

子代理最多跑几步(一步 = 一次模型请求,一步可含多枚工具调用)。不填时用配置的默认:首选 subagent.max_steps_per_turn,未设则继承 max_steps_per_turn(默认 0 = 不限步)。传 0 = 不设上限;剩三步时会收到收口提醒,到限后返回 budget_exhausted 并带回检查点,不会笼统报失败。重试时先读检查点缩小范围,不要原样重发任务、不要擅自抬高步数上限。

## param.agent_type

子代理类型:Explore 只读搜索分析;general-purpose 可做多步操作。默认 general-purpose。

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
