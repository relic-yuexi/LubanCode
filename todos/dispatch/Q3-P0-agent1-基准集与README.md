# 派工单｜Q3-P0-agent1｜基准集脱敏清单与路径表

> 母单：`todos/RankGround边界与瓶颈量化评测_Q3.todo`（第 P0 章）
> 派活形态：one_shot
> 钥匙清单：本单独占 `tests/eval/rankground/datasets/README.md`、`tests/eval/rankground/datasets/manifest.jsonl`、`tests/eval/rankground/datasets/CMakeLists.txt`（仅放数据目录注册）。**不**写图、不写 .hpp/.cpp

## 一、做什么

RankGround 主仓未必在 LubanCode 仓内——P0 不要求把图搬进来，只先把基准集的**清单与脱敏规则**钉死。

四步：

1. 建 `tests/eval/rankground/datasets/` 空目录与 `manifest.jsonl` 占位（暂每条只有 stub）。

2. `manifest.jsonl` 一行一条 JSON，字段：

```json
{
  "id": "dsk-001",
  "组别": "桌面|移动|对抗",
  "原图来源": "内部采集/公开站点（写明 URL 与采集时间）",
  "脱敏规则": "首屏保留 / 全屏打码 / 文本替码",
  "目标区域GT": "目标元素的外接矩形 + 类名（不含敏感字段）",
  "页面特征": {"长宽比": 0.0, "DOM节点数": 0, "滚动高度": 0}
}
```

3. `README.md`：

- 三组比例（100+100+100）的来源规则
- 脱敏三档的具体做法（首屏保留 / 全屏打码 / 文本替码各对应什么场景）
- 公开站点来源白名单（GitHub、各大产品首页、Markdown 渲染站等）
- 标注口径（双人背对背、Kappa ≥ 0.8 才入）
- 不收什么（个人身份、金融交易、医疗记录、内部业务截图）

4. `CMakeLists.txt`：仅一份 `add_custom_target(rankground_datasets_check COMMAND python -m pytest --collect-only)` 占位，本单只让目录进入 CMake 的视野，不真跑数据采集。

## 二、验收

- 三份文件存在、内容齐。
- `manifest.jsonl` 暂为空数组（`[]` 占位）也行——本单只钉 shape，不强求 300 条样本。
- 不开 `-DLUBANCODE_EVAL=ON` 时，CMake 不读到本目录。

## 三、不做什么

- 不下图片、不下真页面（数据采集是 P1 任务）。
- 不写 VLM 推理 hook——agent-2 的活。
- 不写候选区域生成器覆盖率接口——agent-3 的活。
- 不引第三方依赖。

## 四、Worktree 操作

- 从 `origin/main` 开：`git worktree add ../wt-q3-p0-agent1 -b eval/q3-p0-agent1 origin/main`
- 不 merge main、不合 main、不 push。

## 五、报告路径

- 落点：`tests/eval/rankground/datasets/{README.md,manifest.jsonl,CMakeLists.txt}`
- 报告落 `todos/dispatch/Q3-P0-agent1.report.md`
- 不需要编译日志。