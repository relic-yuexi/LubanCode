# agent_watch

## description

查看子代理任务的监督快照(状态、阶段、健康、静默龄、重试数),可等任务发生变化再返回。它不负责故障检测——宿主里的 AgentSupervisor 一直在监督,这里只是把台账账目折成有界只读快照。

只读铁律:不停任务、不传话、不产生任何副作用。要给运行中子代理补充要求,用 agent_message;要停一只任务,用 Agent Dock 的 x;这些都不在本工具。

用法与等待纪律:先不带 wait_ms 查一次,拿到返回里的 revision;之后要等,就把它作 after_revision 传回来,并给 wait_ms(0~30000 毫秒)。修订前进、任务进终态、用户介入、父取消或会话收场都会提前唤醒。状态变化才再等,不要短周期轮询:不要拿 wait_ms=500 之类的短等待循环打卡——等待对宿主零开销,睡到变化即可;反复短轮询只会浪费轮次。

task_ids 不填:main 看全部运行中的根任务,子代理只看自己的直接子任务;显式点名最多 16 只。include=summary(缺省)只回短快照;include=events 另回该任务 after_revision 之后至多 50 枚结构化事件(只有事件类型与工具名,不含正文与思考);include=diagnostic 只对 main 开放,回诊断计数与稳定错误码,同样不含正文、思考、Secret 与完整工具参数。

看结果怎么说:health=healthy 属正常;recovering 表示断流后自动重连中(带第几次);suspect_* 表示宿主已按阶段软线显黄,硬超时与总墙钟照旧兜底,不需要你恐慌性干预。任务进终态(done/failed/cancelled)时结果与短因在 tasks[].state,详细结论等它作为工具结果回流,不要靠本工具反复打探。

## param.task_ids

要查看的任务号(名册里的 #N)。不填 = main 看全部运行中根任务、子代理看自己的直接子任务。最多 16 只;子代理点名了非直接子任务会被拒绝。

## param.after_revision

上次调用返回的 revision。本次快照若与它相同且 wait_ms>0,就睡到任务变化或超时再返回;不同则立即返回新快照。

## param.wait_ms

最多等多久,毫秒,上限 30000;0 = 只取快照不等。等待零开销且有界:用户介入、任务取消、会话收场都会提前唤醒。不要用短 wait_ms 循环轮询——状态变化才再等。

## param.include

输出档位:summary(缺省,状态短快照)/ events(另带结构化事件流,只有类型与工具名)/ diagnostic(只给 main:诊断计数与稳定错误码)。
