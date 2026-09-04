# 派工单｜Q3-P0-agent2｜VLM 推理 hook stub

> 母单：`todos/RankGround边界与瓶颈量化评测_Q3.todo`（第 P0 章）
> 派活形态：one_shot
> 钥匙清单：本单独占 `tests/eval/rankground/vlm_runner.hpp`、`tests/eval/rankground/vlm_runner.cpp`、`tests/eval/rankground/CMakeLists.txt`。**不动** datasets/（agent-1 的活）

## 一、做什么

RankGround 不在 LubanCode 仓内。P0 阶段不接真模型——只把推理 hook 的接口与 stub 落到位。三档硬件 × 三档量化的笛卡尔积在 P3 跑，本单只锁接口形状。

四步：

1. `vlm_runner.hpp`：

```cpp
struct VlmRequest {
  std::string image_path;            // 候选区域截图
  std::string prompt;                // 当前指令
  std::string quantization;          // "fp16" | "int8" | "int4"
  std::string device;                // "rtx4090" | "a100" | "rtx3060" | "cpu"
};

struct VlmTiming {
  std::chrono::milliseconds cold_start;
  std::chrono::milliseconds forward;
  std::chrono::milliseconds io_preprocess;
  std::chrono::milliseconds total;
};

struct VlmResponse {
  std::string text;
  std::optional<std::string> error;
  VlmTiming timing;
};

class VlmRunner {
public:
  virtual ~VlmRunner() = default;
  virtual VlmResponse Infer(const VlmRequest& req) = 0;
};

// Stub：恒返回固定响应 + 固定延迟区间。P3 真机时换实现。
class FakeVlmRunner : public VlmRunner {
public:
  explicit FakeVlmRunner(std::chrono::milliseconds fake_total = std::chrono::milliseconds{145});
  VlmResponse Infer(const VlmRequest& req) override;
};
```

2. `vlm_runner.cpp`：Fake 实现按 `req.quantization` 与 `req.device` 给三档 × 三档的合成延迟（如 int4+cpu 升到 800ms，fp16+4090 落到 145ms）。

3. 单测 `tests/eval/rankground/test_vlm_runner.cpp`：钉三档延迟区间、错误注入路径、取消路径。

4. `tests/eval/rankground/CMakeLists.txt`：

```cmake
add_library(lubancode_eval_vlm_runner STATIC vlm_runner.cpp)
target_include_directories(lubancode_eval_vlm_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

## 二、验收

- 单测 `test_vlm_runner.cpp` 全绿。
- `cmake --build build` 通过；现有 268 个 CTest 全绿。
- 不开 `-DLUBANCODE_EVAL=ON` 时，本目录不被编。

## 三、不做什么

- 不接真 VLM（不引 onnxruntime / libtorch / TensorRT）。
- 不连真硬件（不调 cuda runtime）。
- 不写候选区域生成器覆盖率接口——agent-3 的活。

## 四、Worktree 操作

- 从 `origin/main` 开：`git worktree add ../wt-q3-p0-agent2 -b eval/q3-p0-agent2 origin/main`
- 不 merge main、不合 main、不 push。

## 五、报告路径

- 落点：`tests/eval/rankground/vlm_runner.{hpp,cpp}` / `CMakeLists.txt` / `test_vlm_runner.cpp`
- 报告落 `todos/dispatch/Q3-P0-agent2.report.md`
- 编译日志：`cmake --build build 2>&1 | tee build/build-agent2-rg.log`