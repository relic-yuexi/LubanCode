# Runner 就绪探针与失败原件

本批只改远程 CI 进程夹具。Windows 原件显示：Runner 重启发布 endpoint 时，Python 读取暂报 `PermissionError: [Errno 13]`；失败输出又遇 cp1252 编码错误，原件与总报告未写齐。

## 就绪口径

仍用原有十秒总等待、四十毫秒间隔。进程先退出立即失败。endpoint 尚未出现时继续等；Windows 的 `PermissionError/EACCES` 只在这份已有限时探针内重试。Python 未保留 Win32 原因时，不能认作已证明 sharing violation。永久拒读仍超时失败，最后一次拒读保留诊断。坏 JSON、其他读错照常失败。不放宽十一场断言，不改产品原子替换或状态机。

## 谁留原件

夹具先收 Runner，再把 service 原始字节与已有合成 endpoint/jobs 记录缓存到本场内存；仍不收身份、凭据或任务日志。临时目录清理失败，也用缓存存原件与 UTF-8 traceback。成功场丢缓存，不上传。先存再打印，控制台编码或关闭不得遮住原错。Runner 收场与临时目录清理都办完，才写本场总报告；任一处抛错，改记失败。全部原生进程验收只在远程 CI 跑，本地只跑纯 Python 文件/编码夹具。
