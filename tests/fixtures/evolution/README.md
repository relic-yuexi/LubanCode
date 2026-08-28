# Evolution 夹具

两只 Package Candidate 夹具，对应 `docs/features/evolution/README.md` 冻结的
契约。目录层级照 `~/.lubancode/package-candidates/<package-id>/<candidate-id>/`，
夹具根目录名即 package-id 那一层。

- `candidate-content-only/`：content-only 候选 `cand-20260828-001`。最小包
  （`package.yaml` + `skills/provider-binding-audit/SKILL.md`），评测三行入账，
  `approval.json` 停在 awaiting_approval。
- `candidate-code-rejected/`：带 process Plugin 草稿的候选 `cand-20260828-002`。
  评测虽过，用户以未过 Package 信任门与沙箱核验为由拒绝；拒绝账留 fingerprint。
  草稿不带 runner 实现，`plugin.json` 里的 `${plugin_dir}/runner.py` 故意缺席
  ——带代码候选须另行人工审查构建，不因评测绿而自动补齐，也不得死缠重提。

占位规矩：

- run、goal、recording、memory、feedback 各 ID 一律 `*-placeholder-*`。
- 哈希一律 `sha256:` 接 64 个 0，非真实指纹，只作格式样例。
- 日期统一 2026-08-28。
- 不含真实密钥、账号、Cookie、绝对路径。
