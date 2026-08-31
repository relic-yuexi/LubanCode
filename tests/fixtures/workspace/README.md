# workspace 统一存储夹具(P0-0)

九种用户旧数据场景的**真实脱敏**样本:形状逐字照生产序列化器
(`src/sessions/session_store.cpp`、`src/memory/frontmatter.cpp`、
`src/tools/tool_content.cpp`),内容全为占位——假用户 `sandbox`、假路径、
假 hash、假时间戳。不含任何真实凭据与真实机器路径。

防漂移:`tests/unit/workspace/test_workspace_fixtures.cpp` 用现行 parser
逐件校验;parser 退场时(P0-6)该测试随迁移器测试改吃
`tools/legacy-storage-migrator/` 的隔离副本。

## 清单(详见 manifest.json)

| 场景 | 文件 | 要点 |
|---|---|---|
| 普通会话 | `legacy/plain-conversation.jsonl` | meta 首行 + 纯文本往返 |
| 工具结果 | `legacy/tool-roundtrip.jsonl` | tool_use/tool_result 成对两轮 |
| MCP rich result | `legacy/mcp-rich-result.jsonl` + `mcp-rich-result.mcp-artifacts/` | 五种 rich 块 + structured_content;audio `stored=false` 是故意留的缺口样本 |
| 前台子代理 | `legacy/subagent-foreground.jsonl` | 只有最终回话,无子账(迁移标 `unavailable_legacy`) |
| 后台子代理 | `legacy/subagent-background.jsonl` | 受理回执 + queue 快照 + 完成通知 |
| 压缩 | `legacy/compact.jsonl` | compact_v2(manifest/metrics/epoch/kept_from) |
| 续聊 | `legacy/resume.jsonl` | title/cwd 事件 + 次日尾巴(ts 跳变) |
| linked worktree | `legacy/linked-worktree.jsonl` | cwd 事件进出房;同一 common dir 归同一 workspace |
| 项目 Memory | `memory/project/` | 三种 kind 主题 + `.state/catalog.json` + recall-trace + memory-candidates |
| 全局 Memory | `memory/user/` | preference/feedback 主题 + catalog.json(路径不动的那层) |
| memory job | `memory-jobs/pending/*.json` | pending 态 upsert job |

## 与旧生产目录的映射

```text
legacy/*.jsonl                               -> ~/.lubancode/sessions/<session_id>.jsonl
mcp-rich-result.mcp-artifacts/               -> ~/.lubancode/sessions/<session_id>/mcp-artifacts/
memory/project/{facts,preferences,feedback}/ -> ~/.lubancode/projects/<project_key>/memory/
memory/project/.state/                       -> ~/.lubancode/projects/<project_key>/memory/.state/
memory/project/memory-candidates/            -> ~/.lubancode/projects/<project_key>/memory-candidates/
memory/user/                                 -> ~/.lubancode/memory/user/
memory-jobs/pending/                         -> ~/.lubancode/memory-jobs/pending/
```

配套合同与迁移归宿:`docs/development/workspace-storage-v2/P0-0-contracts.md`
与 `P0-0-inventory.md`。
