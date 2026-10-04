# 安装包消费示例

这份工程只引 `lubancore/core.hpp`，只链接 `LubanCore::Core`。把目录复制到源码树外，给 `CMAKE_PREFIX_PATH` 指向已安装 SDK，便可构建。配置时不查 LubanCode 源码或构建目录。

```sh
cmake -S /path/to/copied/sdk-consumer -B /path/to/consumer-build -DCMAKE_PREFIX_PATH=/path/to/relocated/sdk-prefix
cmake --build /path/to/consumer-build --config Release
ctest --test-dir /path/to/consumer-build -C Release --output-on-failure
```

当前仓库工作流禁止本地编译；上面三步交远端 CI 执行。验收时先安装 SDK，再移动安装目录、复制这份工程，移除原源码/构建路径对消费方配置的帮助。

模型答复由标准库 fixture 提供。会话、Agent 循环、权限、内置文件工具、前台命令、V3 账和持久操作回执都跑真实 SDK。无需模型账号，不发外网请求。

- `smoke`：真实写文件、读文件和前台命令；运行中及完成后重发去重；下一轮带上旧对话；两场会话共用目录并行执行，审批互不串；拒绝、取消、关闭、事件溢出和回调重入；关完句柄后改名数据目录。
- `isolation`：同一项目开两场，另两处项目各开一场。四场并行，逐场核模型、系统提示、历史、工具目录和事件身份；审批、会话放行、取消、关闭互不串。两枚真实工具回调同时扣住，关掉一场后，另一场继续跑；同目录旧场再恢复，旧工具不重跑。另验小回调：缺原会话、必需 MCP 启动失败、参数遭拒，三条退场路都先放回调，再毁后端；每份回调析构都真调用公开后端，后端析构重入关停立刻遭拒。随后正常开场、跑工具、关闭，每份回调析构都能读操作快照，后端仍活。
- `extensions`：只用三份公开头，建场装配 C++ 扩展，实跑输入改写、上下文追加、只读请求挂点与 Next 寿命闸。同目录两场各持实例；恢复重造实例，计划不符则拒绝。另验工厂失败、内联回调清理与后续健康建场。
- `memory-seed` / `memory-resume`：只调公开 SDK，先查空项目库，再召回磁盘主题。第二进程省略 Memory 参数，保留计划与旧报告；改主题只影响未来召回，旧采用正文仍在历史中。重复输入、报告查询和闭场查询都不重跑模型，显式改预算则拒开。同场逐轮报告不借项目“最近一次”报告。
- `seed` / `resume`：CTest 用 fixture 串起两个独立进程。同一 V3 会话恢复后保留旧结果与去重键，旧工具只执行一次，新回合不复用旧回合号。
- `recovery-seed` / `recovery-resume`：首进程让一笔输入停在模型调用中，再受理第二笔，随后用 `_Exit` 跳过析构。新进程恢复后，已派发却没终态那笔仍报 `Indeterminate`；只受理未派发那笔自动续跑，执行一次。

示例只承诺这里列出、远端 CI 实跑通过的范围。文件、Memory 等 fixture 显式传入临时 `resources`；`builtin-search` 另传移位后的安装资源根，实跑随包 ripgrep。示例也不验证模型供应商联网、数据库、远端部署或多用户沙箱。

测试数据放在消费工程构建目录下，每轮用新子目录，不清理调用方已有目录。

- `memory-blobs`：只调公开 SDK，宿主显式提供本场独占 Memory 片段存储。实际提交、采用大片段，再关场、同ID恢复；核原引用、模型正文和旧片段不重写。数据帽与 SHA 校验不当宿主堆内存硬帽。具体承诺见 [Memory 片段 CAS SPI](../../docs/development/sdk-memory-blob-spi.md)。远端原生仍须按当前提交验收。
