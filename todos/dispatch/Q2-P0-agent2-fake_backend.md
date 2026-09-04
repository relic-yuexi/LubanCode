# 派工单｜Q2-P0-agent2｜FakeStreamingBackend 抽出与最小接口

> 母单：`todos/工具与上下文治理量化评测_Q2.todo`（第 P0 章）
> 派活形态：one_shot
> 钥匙清单：本单独占 `tests/eval/support/fake_backend.hpp`、`tests/eval/support/fake_backend.cpp`、`tests/eval/support/CMakeLists.txt`。**不动** `tests/eval/CMakeLists.txt`（agent-1 已写）、不动现有 backend.hpp/cpp、不动 tools/

## 一、做什么

抽一只无网、确定性、可脚本化的 fake backend，给三份实验共用：

- `FakeStreamingBackend`：实现 `api::Backend::send_stream(request, on_event, cancel)`。
- 内置三档 fixture：单文本流、单文本 + 单工具调用流、错误流（timeout / 5xx / 解析错）。
- 输出：流事件按 request 阶段硬编码，**不接网络**。

四步落地：

1. `fake_backend.hpp` 接口：

```cpp
struct FakeBackendScript {
  std::vector<api::StreamEvent> events;     // 逐事件播
  std::optional<api::Error> final_error;   // 末段塞错
  std::chrono::milliseconds per_event_delay{0};
};

class FakeStreamingBackend : public api::Backend {
public:
  explicit FakeStreamingBackend(FakeBackendScript script);
  std::expected<void, api::Error> send_stream(
      const api::Request& req,
      const std::function<void(const api::StreamEvent&)>& on_event,
      const std::atomic<bool>* cancel = nullptr) override;
};
```

2. `fake_backend.cpp` 实现：按 script 播事件，遇 cancel=true 提前收尾，遇 final_error 落 `expected<Error>`。

3. `tests/eval/support/CMakeLists.txt`：

```cmake
add_library(lubancode_eval_fake_backend STATIC fake_backend.cpp)
target_link_libraries(lubancode_eval_fake_backend PUBLIC lubancode_api)
target_include_directories(lubancode_eval_fake_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

4. 在 `tests/eval/CMakeLists.txt` 已经有 `add_subdirectory(support)` 的前提下，**不**修改那个文件——agent-1 落定的 INTERFACE 库已经把 `support/` 包进来，`add_subdirectory` 串会自取本 CMakeLists。

## 二、验收

- 单测 `tests/eval/support/test_fake_backend.cpp`（顺手写了，三档 fixture 各钉一枚）：播完三档事件 + 收尾 + 取消路径。
- `cmake --build build` 通过；现有 268 个 CTest 全绿。
- 不开 `-DLUBANCODE_EVAL=ON` 时，本文件不被编。

## 三、不做什么

- 不动 `src/api/backend.hpp` 与现有三 wire 的 backend。
- 不写 doctest 注册（agent-1 的 INTERFACE 不带 doctest，本单也不主动 discover）。
- 不接真模型、不引 libcurl。
- 不写 collect.py——那是 agent-3 的活。

## 四、Worktree 操作

- 从 `origin/main` 开：`git worktree add ../wt-q2-p0-agent2 -b eval/q2-p0-agent2 origin/main`
- 不 merge main、不合 main、不 push。

## 五、报告路径

- 落点：`tests/eval/support/fake_backend.hpp` / `fake_backend.cpp` / `CMakeLists.txt` / `test_fake_backend.cpp`
- 报告落 `todos/dispatch/Q2-P0-agent2.report.md`
- 编译日志：`cmake --build build 2>&1 | tee build/build-agent2.log`