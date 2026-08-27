# 测试目录规矩

测试按两层落位：先分种类，再分领域。

```text
tests/
  unit/<领域>/test_<名字>.cpp
  integration/<领域>/test_<名字>.cpp
  fixtures/                     固定夹具
  manual/                       刮屏、实验、真服务驱动；不进 CTest
  support/                      doctest 入口与 PCH
```

`unit` 不碰网络，不依赖外部服务，不改进程全局状态。`integration` 可起子进程、
socket 或动态库，须自建临时目录，自选空闲端口，退出前收净句柄与线程。两类测试
都不得依赖执行顺序，也不得借上一册留下的状态过关。

文件名只用 ASCII 小写与下划线。每册只守一门职责。测试标题先写对象，再写场景与
结果，例如：

```cpp
TEST_CASE("RunProcess: 输出超限时收掉进程树并保留截断标记")
```

CMake 会扫描 `unit/*/test_*.cpp` 与 `integration/*/test_*.cpp`。一册源码登记一条
CTest，名字跟路径走。根目录若出现散装 `test_*.cpp`，配置阶段会直接报错。

常用命令：

```bash
# 全跑；失败会报到具体文件
ctest --test-dir build -C Release --parallel 4 --output-on-failure

# 只跑进程领域
ctest --test-dir build -C Release -L process --output-on-failure

# 只跑一册
ctest --test-dir build -C Release -R run_command_process --output-on-failure

# 直接筛一册 doctest，便于传额外参数
build/tests/lubancode_tests --source-file='*test_run_command_process.cpp'
```

写新测试时，先挑种类与领域，再落文件。真机依赖若缺，须明确 `SKIP` 并写出缘故；
断言前用 `CAPTURE` 留下关键路径、退出码与原始输出。莫把 60 秒超时当同步手段，
轮询须有短间隔、硬期限，还要在失败时把现场亮出来。

MiniCPM5-1B 的产品回归不靠部署：脱敏 SSE fixture、上下文预检、错误分型与卡住的
假服务都进 CTest。`manual/` 只放 vLLM Anthropic Messages 真机探针，用来核模型
方言与真机时延；它不进 CTest，只写脱敏统计。复跑须用 PowerShell 7：

```powershell
pwsh -File tests/manual/minicpm5_messages_probe.ps1 `
  -BaseUrl http://localhost:8001 -Model MiniCPM5-1B -ApiKey unused -Repeats 3
```

报告默认落 `build/test-evidence/minicpm5-messages-probe/report.json`。探针不记
key、request id、thinking 正文、signature、图片字节或工具结果正文。
