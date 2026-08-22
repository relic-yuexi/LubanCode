## description

在 shell 里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。shell 参数可选 powershell(默认)或 cmd,分别按对应语法写命令。执行前要经用户确认。超时会被强制杀掉。起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)查状态/读输出,stop_background 工具收尾。

## description (POSIX)

在 shell(/bin/sh)里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。按 POSIX sh 语法写命令。执行前要经用户确认。超时会被强制杀掉。起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)查状态/读输出,stop_background 工具收尾。

## param.command

要执行的命令,按所选 shell 的语法写(默认 PowerShell 语法)

## param.command (POSIX)

要执行的命令,按 POSIX sh 语法写

## param.timeout_ms

超时时间,单位毫秒,不填默认 120000(2 分钟)

## param.shell

用哪个 shell 执行,不填默认 powershell

## param.shell (POSIX)

用哪个 shell 执行,本平台只有 sh(/bin/sh);powershell/cmd 是 Windows 专属

## param.run_in_background

true = 后台运行,不等命令跑完就返回。用于起 dev server、watch 进程这类要跨命令、跨多轮调用继续存活的长命进程——起完之后你还要接着用别的命令(比如 curl)去验证它;也用于后台跑一个短任务,不想阻塞当前对话、跑完通知你即可。spawn 成功后立刻返回结果,内含 task_id、子进程 PID 和一个日志文件路径(该进程的标准输出/标准错误合并写在这个文件里);命令跑完时,下一次给提示符会打一行完成通知。之后想看它是否还活着、看它吐了什么,用 background_output 工具(传 task_id)查状态读输出,要收掉它就用 stop_background 工具。timeout_ms 参数对这个模式没有意义,会被忽略。不填默认 false(前台执行,等命令跑完拿完整输出和退出码)。

## param.cwd

命令的工作目录,相对或绝对均可;不填用当前会话工作目录。目录必须真实存在。住隔离 worktree 的会话里,指向主 checkout 的目录会被拒绝
