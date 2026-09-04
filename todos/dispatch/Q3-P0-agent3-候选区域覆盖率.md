# 派工单｜Q3-P0-agent3｜候选区域生成器覆盖率记录接口

> 母单：`todos/RankGround边界与瓶颈量化评测_Q3.todo`（第 P0 章）
> 派活形态：one_shot
> 钥匙清单：本单独占 `tests/eval/rankground/candidate_coverage.hpp`、`tests/eval/rankground/candidate_coverage.cpp`、`tests/eval/rankground/candidate_coverage_stub.cpp`。**不动** vlm_runner.* 与 datasets/

## 一、做什么

把"候选区域生成器命中数 / 漏检 / IoU"这套记录的接口形状钉死。本单只落 stub——真区域生成器在 P3 接入，本单先把 ledger 与数据形状钉到位，让 P1 实验 A 跑起来时不至于改接口。

四步：

1. `candidate_coverage.hpp`：

```cpp
struct CoverageRecord {
  std::string dataset_id;            // manifest.jsonl 的 id
  std::string group;                 // 桌面|移动|对抗
  std::vector<std::string> hit_regions;     // 命中区域名
  std::vector<double> iou_per_region;       // 与 GT 的 IoU
  std::optional<std::string> miss_reason;   // 未命中的页面特征说明
  std::chrono::milliseconds generation_latency;
};

class CandidateCoverageLedger {
public:
  void Record(const CoverageRecord& rec);
  // 按 (group, hit/miss) 分桶聚合
  std::map<std::string, std::size_t> HitMissByGroup() const;
  double MedianIoU(const std::string& group) const;
  // 导出 JSON 行（与 manifest.jsonl 同源）
  std::string ExportJsonl() const;
};
```

2. `candidate_coverage.cpp` 实现。

3. `candidate_coverage_stub.cpp`：提供一个示例 stub——把一张桌面页面 + 一张对抗页面的假数据灌进 ledger，让单测能直接断言聚合形态。

4. 单测 `tests/eval/rankground/test_candidate_coverage.cpp`：三组（桌面/移动/对抗）各喂 5 条记录，断言聚合桶数、IoU 中位数、ExportJsonl 的字段齐。

## 二、验收

- 单测 `test_candidate_coverage.cpp` 全绿。
- `cmake --build build` 通过；现有 CTest 全绿。
- 不开 `-DLUBANCODE_EVAL=ON` 时，本目录不被编。

## 三、不做什么

- 不写候选区域生成器本身（与 RankGround 主仓强耦合，P3 才接）。
- 不接真图、不跑真推理。
- 不写 collect.py——Q2 的 agent-3 写过一份通用模板，Q3 的 collect.py 留 P1。

## 四、Worktree 操作

- 从 `origin/main` 开：`git worktree add ../wt-q3-p0-agent3 -b eval/q3-p0-agent3 origin/main`
- 不 merge main、不合 main、不 push。

## 五、报告路径

- 落点：`tests/eval/rankground/candidate_coverage.{hpp,cpp}` / `candidate_coverage_stub.cpp` / `test_candidate_coverage.cpp`
- 报告落 `todos/dispatch/Q3-P0-agent3.report.md`
- 编译日志：`cmake --build build 2>&1 | tee build/build-agent3-rg.log`