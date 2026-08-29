# Evolution 夹具

三只 Package Candidate 夹具，对应 `docs/features/evolution/README.md` 冻结的
契约。目录层级照 `~/.lubancode/package-candidates/<package-id>/<candidate-id>/`，
夹具根目录名即 package-id 那一层。

- `candidate-content-only/`：content-only 候选 `cand-20260828-001`。最小包
  （`package.yaml` + `skills/provider-binding-audit/SKILL.md`），评测三行入账，
  `approval.json` 停在 awaiting_approval。
- `candidate-code-rejected/`：带 process Plugin 草稿的候选 `cand-20260828-002`。
  评测虽过，用户以未过 Package 信任门与沙箱核验为由拒绝；拒绝账留 fingerprint。
  草稿不带 runner 实现，`plugin.json` 里的 `${plugin_dir}/runner.py` 故意缺席
  ——带代码候选须另行人工审查构建，不因评测绿而自动补齐，也不得死缠重提。
- `candidate-eval-smoke/`：阶段 3 评测形状样板 `cand-20260828-003`（包
  `evolve.eval-smoke`）。`eval-plan.json` 带夹具工作区
  （`fixtures/replay-workspace/report.json`）、对象式可执行验收与人工字符串
  验收混排、`baseline.fixture` 指向 `fixtures/baseline-bare.json`（裸 Agent
  指标账）。内容哈希 64 个 0 占位，真跑评测会因哈希对不上拒评——执行型
  测试在 `tests/unit/evolution/test_evolution_eval.cpp` 里用临时目录现算哈希
  现写计划，不在夹具上留一改就失效的硬哈希。

占位规矩：

- run、goal、recording、memory、feedback 各 ID 一律 `*-placeholder-*`。
- 哈希一律 `sha256:` 接 64 个 0，非真实指纹，只作格式样例。
- 日期统一 2026-08-28。
- 不含真实密钥、账号、Cookie、绝对路径。
- 验收命令只放跨平台无害的（文件存在/JSON 可解析一类），不起危险进程。
