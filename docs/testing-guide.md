# 测试指南

[文档首页](README.md) · [开发指南](development-guide.md) · [终端交互](terminal-ui.md) · [文档规范](documentation-standard.md)

LubanCode 的风险不只在函数返回值。协议会断包，进程会超时，终端会残影，队列会跨线程，外部服务还会撒谎。这页把测试分层，说明何时跑哪一层，哪些结果能写进公开文档。

## 1. 五层测试

| 层 | 目标 | 网络 | 默认 CI |
| --- | --- | --- | --- |
| 纯逻辑 | 解析、状态机、预算、格式、合并 | 不要 | 是 |
| 组件/进程夹具 | 假 SSE、假 MCP/LSP、临时目录、真子进程 | 本机 loopback | 是 |
| 应用集成 | InteractiveSession、工具链、存档、配置写回 | 通常不要 | 是 |
| 真终端 | Win32 刮屏、键位、viewport、残影 | 假本地服务 | 否，手动 |
| 真服务/基准 | Provider 兼容、usage、缓存、PTC 效果 | 要 | 否，opt-in |

能在低层钉死的，不推给高层。真服务测试用来补兼容证据，不替代离线回归。

## 2. 构建与全套测试

### Windows

```powershell
cmake --preset release
cmake --build --preset release --target lubancode_tests
ctest --test-dir build\release -C Release --output-on-failure
```

直接跑 doctest 总程序：

```powershell
.\build\release\tests\Release\lubancode_tests.exe --no-skip
```

### Linux / macOS

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target lubancode_tests -j 4
ctest --test-dir build --output-on-failure
```

测试总目标与源文件清单以 `tests/CMakeLists.txt` 为准。文档不硬写用例总数；数字会随提交变化。

## 3. 跑窄测试

doctest 可按测试名过滤。先列出名字：

```powershell
.\build\release\tests\Release\lubancode_tests.exe --list-test-cases
```

再跑一组：

```powershell
.\build\release\tests\Release\lubancode_tests.exe --test-case="*LineEditorCore*"
```

过滤词应指向行为，不要只指文件名。修一处 bug，至少要有一条“修前失败、修后通过”的断言。

## 4. 测试放哪

| 改动 | 首选测试 |
| --- | --- |
| JSON/CLI 解析 | 同模块 `test_*.cpp` 纯函数用例 |
| 配置覆盖 | `test_config.cpp` 与专题配置测试 |
| 请求/事件 | 各 wire 的 request/events fixture |
| AgentLoop | 捕获 backend + 假工具 |
| 文件/进程 | 临时目录、假命令、超时与取消 |
| Hook | event、matcher、归并、退出码与 trust fixture |
| Memory | 候选、召回、worker、指纹、预算与中文语料 |
| PTC | protocol、stub、runner、tool、profile、bench 分层 |
| CLI 状态机 | `test_line_editor.cpp`、面板/footer 纯状态测试 |
| 真屏幕 | 对应 `*_driver` |

共享夹具放 `tests/fixtures/`。测试不可依赖用户真实 `~/.lubancode`、当前仓库记忆、线上会话或个人 key。

## 5. 失败态先于成功态

至少考虑：

- 空值、未知枚举、越界数字、坏 UTF-8。
- HTTP 非 2xx、SSE 半包、usage 缺席、服务端提前断流。
- 工具参数错、确认拒绝、超时、取消、输出过大。
- 文件消失、路径逃逸、同名冲突、并发写。
- session 坏尾行、compact 失败、恢复旧格式。
- 终端 resize、窄屏、中文宽字符、焦点切换、插打残影。
- 子代理目标已结束、消息未送达、主回合中断。

成功路只证明“晴天能走”。失败路才守住数据。

## 6. 真终端驱动

Windows 下的 `screen_driver`、`stream_footer_driver`、`agent_panel_driver`、`agent_stream_driver`、`viewport_driver` 等均为 `EXCLUDE_FROM_ALL`。点名构建：

```powershell
cmake --build build\release --config Release --target agent_stream_driver viewport_driver
```

驱动参数以 `tests/CMakeLists.txt` 与各文件顶部说明为准。工作目录应指向新建的临时夹具，报告也落临时目录。驱动要断言：

- 行序与边框位置。
- composer、状态栏和面板是否仍在可视区。
- 切换只改选中态，按 Enter 才换视图。
- 完成项是否退场，旧 footer 是否清净。
- resize、滚屏与中文宽字符后是否留残影。

截图供人复核，像素或行文本断言才是回归证据。只拍一张“看着不错”不够。

## 7. 真服务测试

真服务一律 opt-in：

- key 走环境变量。
- 临时 home 与临时项目隔离。
- 不改用户默认 provider、模型、记忆与 session。
- 记录 endpoint、wire、模型、日期、commit 与请求参数。
- 报告服务端原始 usage 与客户端归一值，两者分开。
- 超时、限流与供应商波动不得误判成代码回归。

`deepseek_e2e` 是现有示例：没设 `DEEPSEEK_API_KEY` 时自行跳过，不进 ctest。PTC 真模型探针与手测矩阵见 [PTC 手册](ptc.md)。

## 8. 多轮会话测试

凡是要证明历史、缓存、compact、memory 或子代理接续，不能只打一枪。最少三轮：

1. 第一轮建立事实或工具结果。
2. 第二轮引用前史并再调用工具。
3. 第三轮不重读，检查能否凭历史收口。

同时记录每轮模型请求数。三条用户消息不等于三次请求；工具往返会增加 step。术语按[命名与计数规范](naming-conventions.md)。

## 9. 缓存测试口径

缓存测试须分：

- 冷启动：服务端缓存已清，或使用从未出现的稳定前缀。
- 热重复：请求完全相同。
- 热前缀：只改尾部，检查旧前缀能否命中。
- 多轮：历史增长后逐轮取 metrics/usage 增量。

每轮采集前后差值，不拿整台共享服务的累计计数冒充本次结果。供应商 usage、Prometheus metrics、客户端估算各列一栏。若 Chat usage 报零而服务端 metrics 有命中，结论只能写“usage 未回报缓存细账”，不能写“缓存为零”或“客户端测得命中”。

公开结果至少跑预热与多次正式样本，报 P50/P95。单次截图留在测试记录，不进产品 README。

## 10. 性能与编译时间

测编译时先分 Configure、Build、Test。Windows CI 慢，常见主因在编译与链接，不要看总时长便怪测试。

记录：

- clean build 还是 incremental build。
- 目标是 `lubancode`、`lubancode_tests` 还是两者。
- 编译器、配置、并行度、vcpkg/FetchContent 路线。
- PCH 是否命中，改动是否触发公共头重编。
- 中位数与波动，不只报一场 GitHub Actions。

要优化时，先从 job 分段日志找最长一段，再用编译器 trace 或 CMake timing 定位。测试数多不等于 Build 变慢；两者须分开量。

## 11. CI

`.github/workflows/ci.yml` 在三套 runner 上：

1. checkout。
2. vcpkg configure，失败回退 FetchContent。
3. 分别构建 `lubancode` 与 `lubancode_tests`。
4. `ctest --output-on-failure`。

当前 workflow 对 `docs/**`、`todos/**` 与双语 README 做 `paths-ignore`。纯文档提交不会跑代码 CI。代码提交必须让三路都绿；平台专属代码不能只在本机编过便算完。

## 12. 提交前清单

- [ ] 窄测试覆盖修复点。
- [ ] 失败态与取消态有断言。
- [ ] 共享状态有并发或生命周期测试。
- [ ] 用户目录、网络与 key 已隔离。
- [ ] 需要时跑了全套 ctest。
- [ ] 终端改动跑了对应 driver。
- [ ] 真服务结果标成 opt-in，不混入默认测试数。
- [ ] 文档里的数字带复测口径。
- [ ] `git diff --check` 通过。
