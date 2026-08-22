## description

查后台命令(run_command run_in_background:true 起的那些)的运行状态和输出。不给 task_id 就列出全部后台任务的摘要:task_id、状态(运行中/完成/失败/已停止)、命令、PID、日志文件路径。给 task_id 就返回该任务的详情 + 日志文件尾部 tail_lines 行(默认 50)。任务还在跑也能读,文件允许边写边读。起完一个后台命令后,用它查进度/结果,不用自己再拼 tail 命令。

## param.task_id

要查的后台任务 id(run_command 后台返回的那个编号字符串)。不给就列出所有后台任务的摘要。

## param.tail_lines

读日志文件的末尾几行,默认 50。给 task_id 时才用;<=0 表示读全文(上限 64KB)。
