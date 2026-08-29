# dies-loud 的坏件脚本:启动即退非零,一行协议不回,什么也不干(无害)。
# 挂载事务的探针起这只进程,收到的是非零退出码——按 ToolExitNonZero 拦下,
# 整包回滚。
import sys

sys.stderr.write("dies-loud: 夹具坏件,启动即退 3。\n")
sys.exit(3)
