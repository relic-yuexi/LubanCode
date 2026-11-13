# Workspace racer 首次 ready 诊断

原件来自 81ed65f2 Windows full：worktree_shared_storage 在 held 案首次读取 ready 时没有得到 ok 前缀。正文与子进程结果缺失，尚未定因。本笔从 605d200f 起，只补 test/helper 可观测原件和异常退场。

## 首观察与句柄

held 案保原 spawn、20s exists 等待及首次字节前缀判定。第一次读取同时保存 open/eof/fail/bad、原字节十六进制和路径；后来的文件状态与日志都单列，不许取后来 ok 改判。没有重试读取、等待完整正文、预热或重启 helper。

test 中的 RAII 观察对象持同一 BackgroundProcessHandle 的 shared owner，连同原 argv、ready/release 路径和后台合并日志路径。原正常 release 与退出等待照旧；若首次观察不合，先保存自然 Wait(0)/Peek 快照，再发这只 hold helper 的 release，沿既有 20s 退出口收场，仍未退出才调用既有 TerminateTree(0) 并再核非阻塞 Wait/Peek。记录动作与实际结果，不能把清场退出码当成自然成功。注册模式的 go 会触发业务写入，本笔不把它当通用清场令。

原首观察失败保持失败。fatal 前输出清场回执；中途别的 fatal 由 RAII 收同一只 helper。unknown 完成态的 exit_code 记 null，不拿默认零补成功。stdout/stderr 原本共用一个日志文件，只记录 combined 字节或读取缺口，不伪拆两流。日志路径可能缺件也如实报告。

## helper 原件

helper 保原 Acquire、状态分支、ready 字节、等待与退出码。只在真实调用后记录 Acquire status/detail/holds/耗时，以及原 ready 写入的 open、write、close 观察。把原作用域末尾关闭改为同处显式 close，以便读关闭状态；不加 flush/fsync、原子替换或重试。诊断用独立前缀写入原 stderr，保持 ready 协议不变；崩溃模式仍是 _Exit(9)。

## 保留边界

只动这份合同、test_worktree_shared_storage.cpp 和 workspace_manifest_racer.cpp。原六 CASE 保留，其余五案正文原样；held 案原 manifest 状态、workspace.locked 和放行后接手断言不删不降。Acquire 60×100ms、ready/held exit 20s、helper 120s、crash 30s、CTest 300s 不变。生产 lock/process 源、CMake 与 CI 不动。本地只查源码与纯数据，不配置、编译或执行原生；这一笔不宣称已修根因。
