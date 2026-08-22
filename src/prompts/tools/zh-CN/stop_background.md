## description

停掉一个后台命令(run_command run_in_background:true 起的)。Windows 上 TerminateProcess 根进程,POSIX 上 kill 杀整个进程组。已完成的任务不会重复杀。长命进程(dev server、watch、build)跑够了、或者起错了想收掉,用它。

## param.task_id

要停的后台任务 id(run_command 后台返回的那个编号字符串)。
