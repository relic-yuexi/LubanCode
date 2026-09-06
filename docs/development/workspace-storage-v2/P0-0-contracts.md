# Workspace 统一存储 P0-0 冻结合同:schema、版本与错误码

- 日期:2026-09-01
- 状态:**冻结**。P0-1 起各波次照此实现;改合同先改本文与
  `src/workspace/storage_contracts.hpp`(纯头文件常量,本批不接线),再动码。
- 单子:`todos/Workspace统一存储_旧Session退场与分级Memory迁移.todo` §三/§四/§六/§七/§九。
- 约定:样例里时间戳、hash、id 都是占位形状,不是真值;路径一律正斜杠
  (落盘前按平台转换)。

---

## 一、目录布局(v2 唯一持久化树)

```text
~/.lubancode/
  workspaces/                        # 唯一项目持久化根
    index.json                       # 目录账本(见下;可重建缓存,非真账)
    <门牌>/                          # 目录名 = 路径slug + seed哈希前8,≠workspace_key
      workspace.json                 # 见 §二(v2 manifest)
      sessions/
        <session_id>/
          session.json               # 见 §三(场 manifest)
          main.jsonl                 # 主会话唯一真账(typed 事件流)
          subagents/<agent_run_id>.jsonl
          workflows/<workflow_run_id>/{workflow.jsonl,nodes/,checkpoints/}
          goals/
          loops/
          checkpoints/
          artifacts/sha256/<sha256>.<ext>   # 内容寻址:MCP rich、模型图片、上下文仓 blob、recall snapshot
          indexes/                   # 可重建索引(会话列表、提问历史、usage)
          derived/                   # 可重建投影
          exports/                   # /export 出档
      memory/
        facts/ preferences/ feedback/ archive/ memory-candidates/
        index.md                     # 人看,不进 prompt
        .state/{catalog.json, recall-traces/, memory.lock}
      lifecycle/<operation_id>/{intent.json,result.json}
      tombstones/<session_id>.json
      .locks/
  memory/user/                       # 全局记忆:无 workspace_key,路径不动
    preferences/ feedback/ archive/
    index.md
    .state/catalog.json
  memory-jobs/{pending,running,failed}/   # 全局件;P0-3 起项目类 job 路由进 workspace
  migrations/storage-v2/<operation_id>/   # 见 §五
      {intent.json, progress.json, result.json}
```

### 目录账本(`workspaces/index.json`,账本制)

目录名是**门牌**(装饰),查找走账本——路径为键、门牌为值:

```json
{
  "schema": "lubancode.workspace-index",
  "version": 1,
  "workspaces": {
    "d:/work/demo-repo/.git":  { "dir": "D--work-demo-repo-3f2a9c1b", "created": 1767225600000 },
    "d:/mineru/2604.10547v2":  { "dir": "D--MinerU-2604-10547v2-e5f6a7b8", "created": 1767312000000 }
  }
}
```

- **键**:canonical 路径串——identity 现行 seed 归一(正斜杠、去尾斜杠、
  Windows 折 ASCII 小写),git 身份取 common git dir(主树与 linked
  worktree 同键同房);
- **值**:`dir` = 漂亮门牌 + `created` epoch 毫秒;
- **找门三步**:查账 → miss 生门牌开房(各房 `workspace.json` 照写,自
  描述保留)→ 记账(原子写);账本缺/坏 → 扫各房 `workspace.json` 自描述
  重建,不走兼容路;
- 门牌 = `<路径slug(≤80字节)>-<seed 的 SHA-256 前 8 hex>`。slug:盘符
  `D:` → `D--`;分隔符与 `.`、空格 → `-`;非法字符 `:*?"<>|` 与控制符 →
  `_`;中文/Unicode 原样保留;超 80 字节截断(UTF-8 边界回退);Windows
  保留名(CON/PRN/NUL/COM1-9…)前缀 `_`。哈希段保唯一——不同 seed 必不同
  门牌;
- 身份仍是 `workspace_key`(manifest/session.json 里的那枚),门牌不参与
  裁决;按 key 反查房门走扫房 manifest 自描述,不靠目录名。

硬规矩(单子 §三):`sessions/`、`trajectories/`、`projects/` 三个生产目录
退出;索引/导出/摘要可重建;账本同属可重建;Journal、Memory Markdown
正文、用户确认过的迁移回执才是真本。

## 二、workspace v2 manifest(`workspace.json`)

```json
{
  "schema": "lubancode.workspace",
  "version": 2,
  "workspace_key": "demo-repo-3f2a9c1b7d8e4025",
  "display_name": "demo-repo",
  "identity_kind": "git_common",
  "identity_root": "C:/Users/sandbox/work/demo-repo/.git",
  "created_at_ms": 1767225600000,
  "last_opened_at_ms": 1767312000000,
  "checkouts": [
    {"root": "C:/Users/sandbox/work/demo-repo", "first_seen_at_ms": 1767225600000, "last_seen_at_ms": 1767312000000}
  ],
  "migrated_from": {"old_project_key": "demo-repo-1a2b3c4d5e6f7088", "at_ms": 1767225600000}
}
```

| 字段 | 中文 | 规矩 |
|---|---|---|
| `schema` | schema 名 | 恒 `"lubancode.workspace"` |
| `version` | 版本 | 恒 `2`;reader 见大于已支持的 version 一律 `schema.unsupported_version` 拒读,不猜 |
| `workspace_key` | 工作区钥匙 | 与算法重算结果必须逐字相同,不合即 `identity.key_mismatch` 隔离 + doctor,不自动改名 |
| `display_name` | 显示名 | 只给人看,不参与唯一性 |
| `identity_kind` | 识别来源 | `git_common` \| `explicit_marker` \| `config_root` \| `cwd_fallback`,四值封闭 |
| `identity_root` | 身份规范根 | 算 key 的根:Git 取 common git dir;marker/config/cwd 取对应目录 |
| `created_at_ms` / `last_opened_at_ms` | 建/开时刻 | epoch 毫秒 |
| `checkouts[]` | 检出登记 | 可重建,非身份源;`root`/`first_seen_at_ms`/`last_seen_at_ms` |
| `migrated_from` | 迁移来源 | 可选;只有 P0-5 迁移器写,记旧 project_key 与时刻 |

首次开仓原子写(tmp + rename);路径搬家不凭同名目录自动并账,走显式
`lubancode workspace migrate`。

## 三、session manifest(`session.json`,v2)

沿用 trajectory v1 字段,`schema_version` 升 2,新增两键:

```json
{
  "schema_version": 2,
  "workspace_key": "demo-repo-3f2a9c1b7d8e4025",
  "session_id": "20260115-093000-x7k2qf",
  "launch_cwd": "C:/Users/sandbox/work/demo-repo",
  "main_run_id": "run-20260115-093000-a1b2c3",
  "start_reason": "legacy_import",
  "previous_session_id": null,
  "status": "completed",
  "created_at_ms": 1767225600000,
  "lubancode_version": "0.27.0",
  "event_schema_version": 1,
  "subagent_detail": "unavailable_legacy",
  "training_policy": "exclude"
}
```

| 新键 | 中文 | 规矩 |
|---|---|---|
| `subagent_detail` | 子账明细度 | 仅 `start_reason=legacy_import` 场必填,恒 `unavailable_legacy`(旧主账只有 agent 最终回话,不伪造子 Journal) |
| `training_policy` | 训练策略 | 仅迁移场必填,恒 `exclude`;复现等级不高于 `partial` |

`start_reason` 枚举冻结:`process_launch` | `clear` | `resume` | `legacy_import`
(前三个沿用现状值,一字不改)。新根下读到 `schema_version:1` 即旧档搬错家,
doctor 报 `session.journal_corrupt`。

## 四、memory recall snapshot(`context.injected` 事件 + artifact 双形态)

每次真正注入模型的记忆,`main.jsonl` 必落一枚:

```json
{
  "kind": "run.event",
  "event": "context.injected",
  "payload": {
    "kind": "memory_recall",
    "memory_level": "project",
    "memory_id": "fact.build-cmd-001",
    "memory_schema": 3,
    "memory_updated_at": "2026-01-15T09:30:00Z",
    "content_sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
    "source_evidence_refs": ["workspace_key=e2e/session_id=20260115-093000/run_id=run-x/event_id=run-x:evt-00000012"],
    "injected_bytes": 412,
    "snapshot_ref": "artifacts/sha256/9f86d081...txt"
  }
}
```

| 字段 | 中文 | 规矩 |
|---|---|---|
| `kind`(payload 内) | 注入类别 | 恒 `memory_recall`;将来别的注入类别另开枚举值 |
| `memory_level` | 记忆层级 | `project` \| `user`;user 层只在用户全局授权召回时出现 |
| `memory_id` | 主题 id | 旧 id 原样(前缀 fact./preference./feedback.) |
| `memory_schema` | 主题 schema | 主题当下的格式号(1/2/3) |
| `memory_updated_at` | 主题更新时刻 | 注入时所读版本的 updated_at |
| `content_sha256` | 正文指纹 | 注入正文的 SHA-256(hex64) |
| `source_evidence_refs` | 来源证据引用 | 全限定引用数组,元素形状见 §六 |
| `injected_bytes` | 注入字节数 | 正文 UTF-8 字节数 |
| `snapshot_ref` | 快照引用 | 指向本 session `artifacts/sha256/` 的相对路径;≤512B 的小内容允许内联 `snapshot_inline` 替代此键 |

Memory 写入因果边(三事件,P0-3 接线):

```text
memory.save.requested  payload: {source_session, source_run, source_turn, source_event_ref, request}
memory.save.committed  payload: {memory_id, memory_version, content_sha256, memory_path, committed_at}
memory.save.failed     payload: {stable_error_code, retryable}
```

快照写不稳时:本轮不注入该记忆或明确阻断(`memory.recall_snapshot_failed`),
不得"注了却无账"。

## 五、迁移回执(`migrations/storage-v2/<operation_id>/`)

### intent.json(抢占即锁,同 operation_id 只许一个 committed 结果)

```json
{
  "schema": "lubancode.storage-migration",
  "version": 1,
  "operation_id": "mig-20260115-100000-x7k2",
  "created_at_ms": 1767225600000,
  "source": {
    "kind": "sessions",
    "roots": ["C:/Users/sandbox/.lubancode/sessions"],
    "files": [
      {"path": "20260115-093000-hello.jsonl", "bytes": 2048, "sha256": "…hex64…", "meta_cwd": "C:/Users/sandbox/work/demo-repo"}
    ]
  },
  "planned": [
    {"source_sha256": "…hex64…", "workspace_key": "demo-repo-3f2a9c1b7d8e4025"}
  ]
}
```

### progress.json(崩溃续跑依据,每文件落一笔后原子替换)

```json
{"schema": "lubancode.storage-migration", "version": 1, "operation_id": "mig-…",
 "phase": "importing", "done": 3, "total": 9, "updated_at_ms": 1767225612000,
 "last_source_sha256": "…hex64…", "last_outcome": "imported"}
```

### result.json(原子写;只有它算 committed)

```json
{
  "schema": "lubancode.storage-migration",
  "version": 1,
  "operation_id": "mig-20260115-100000-x7k2",
  "started_at_ms": 1767225600000,
  "finished_at_ms": 1767225700000,
  "items": [
    {
      "source_sha256": "…hex64…",
      "source_path": "20260115-093000-hello.jsonl",
      "outcome": "imported",
      "target_session_id": "20260115-100100-m1g2a3",
      "target_workspace_key": "demo-repo-3f2a9c1b7d8e4025",
      "terminal_event_hash": "…hex64…",
      "legacy_partial": true,
      "subagent_detail": "unavailable_legacy",
      "missing": ["rich block art-0002.png bytes", "subagent journals"]
    }
  ],
  "counts": {"imported": 8, "already_imported": 1, "skipped_unreadable": 0, "failed": 0},
  "training": "exclude",
  "source_deleted": false
}
```

| 字段 | 中文 | 规矩 |
|---|---|---|
| `outcome` | 单件结果 | `imported` \| `already_imported`(同 source SHA 重跑返回既有目标,不再造一份) \| `skipped_unreadable` \| `failed` |
| `legacy_partial` | 部分导入 | 有不可还原边界即 true |
| `missing[]` | 缺口清单 | 照实列(rich block、artifact、子账、原始 stdout 截断),不冒充已恢复 |
| `terminal_event_hash` | 终态指纹 | 目标场 terminal 事件的 event_hash,核验用 |
| `source_deleted` | 源档处置 | 默认 false;`--delete-source` 二次确认且复验通过才 true |

## 六、全限定引用形状(单子 §6.3)

Memory 的 `source_sessions` 与 recall snapshot 的 `source_evidence_refs`
统一用:

```text
workspace_key=<key>/session_id=<id>/run_id=<id>/event_id=<run>:evt-%08llu
```

旧式裸 session id 只许出现在迁移器输入侧;P0-3 起生产写入一律全限定。

## 七、reader 最低版本与版本协商规矩

1. **每份 JSON 带 schema 标识**:`schema`(名) + `version`(数),或既有
   `schema_version` 单键(session.json/event envelope 沿用)。事件信封另钉
   `event_schema_version` 进 session manifest(现状语义保留)。
2. **reader 最低版本**:workspace v2 树内全部文件按 `*_MinReaderVersion`
   (见 `storage_contracts.hpp`)声明;当前全部为 2(workspace/session)或
   1(migration/recall,新件首版)。
3. **协商规矩(定死,不留暗门)**:
   - 文件 `version` ≤ reader 上限:照读;缺键按该 schema 默认值兜(现状
     v1 老档语义)。
   - 文件 `version` > reader 上限:`schema.unsupported_version` 拒读整份,
     **不猜、不静默降级、不部分解析**。
   - `version` 缺失或非整数:按该 schema 的"缺省版"处理,并记
     `schema.missing_field`(诊断级);迁移器遇到缺省版旧档按 1 兜。
   - 事件 envelope 未知 `kind`:现状码 `schema.kind_not_in_version`,reader
     停在该事件,不停整条流(Replay 现状语义,保留)。
4. **旧根数据只许迁移器读**:生产 reader 对 `~/.lubancode/sessions/`、
   `trajectories/`、`projects/` 零读零写(P0-6 起静态审计保证)。

## 八、稳定错误码清单(冻结:只加不改不删)

风格:点分 `<域>.<原因>`,小写。常量在 `src/workspace/storage_contracts.hpp`。

### 本批新增

| 错误码 | 中文场景(§9.2 对应) |
|---|---|
| `identity.no_boundary` | 身份算不出:启动失败并点明路径,不落 unknown/ 总筐 |
| `identity.key_mismatch` | manifest 与算法 key 不合:隔离 + doctor,不自动改名合并 |
| `identity.path_invalid` | key/session id/run id 不是合法单段名 |
| `workspace.not_found` / `workspace.open_failed` / `workspace.locked` | workspace 缺失/开张失败/锁被占 |
| `workspace.permission_denied` | 目录收紧 user-only 失败或越权路径 |
| `workspace.disk_full` | 预留空间不足,禁回退旧 SessionStore |
| `session.not_found` / `session.open_failed` / `session.locked` | 场缺失/开张失败/同场抢写 |
| `session.journal_corrupt` | Journal 坏(尾行截断按现状停在最后完整事件,整份坏才用此码) |
| `session.parent_edge_broken` | 父子双向 hash 校验不合,标 incomplete/corrupt |
| `session.write_failed` | 关键事件写不稳:停在耐久边界,不越下一副作用 |
| `session.migration_pending` | 迁移未完成的场不可当正常场用 |
| `memory.save_failed` | 项目 Memory 写失败:job 进 failed,可重试 |
| `memory.recall_snapshot_failed` | recall snapshot 写不稳:本轮不注入或阻断 |
| `memory.global_unauthorized` | 全局层未获用户主动授权:拒绝写入,项目配置不得提权 |
| `memory.job_failed` | 后台 worker 回执失败态 |
| `migration.intent_exists` | operation_id 复用 |
| `migration.result_exists` | 同 source SHA 已 committed |
| `migration.source_sha_mismatch` | 源档 hash 与 intent 不符(迁移中被改动) |
| `migration.source_unreadable` | 源档读不出(记录后跳过,不中断整批) |
| `migration.target_write_failed` | 目标写坏:旧源不动,可续跑 |
| `migration.interrupted` | 迁移中断:按 progress/hash 幂等续跑 |
| `migration.delete_unverified` | 未核验/未二次确认的删源请求 |

### 沿用既有(不重编,此处列防撞名)

`schema.unsupported_version`、`schema.kind_not_in_version`、
`schema.payload_*`、`schema.missing_field`、`schema.bad_seq`、
`schema.bad_event_id`、`schema.bad_hash`、`schema.plane_mismatch`、
`schema.bad_actor_origin`(trajectory/schema.cpp);
`session.lifecycle_intent_failed`、`session.lifecycle_result_failed`、
`lifecycle.intent_exists`、`lifecycle.result_exists`、`lifecycle.tombstone_exists`
(session_manager.cpp);
`resume.source_not_found`、`resume.source_corrupt`、`resume.source_locked`、
`resume.source_invalid_ref`、`resume.source_unsupported`、`resume.busy`、
`clear.busy`、`clear.no_active_session`、`close.busy`、`close.no_active_session`、
`verify.bad_line`、`verify.empty_line`、`verify.truncated_tail`、
`replay.unsupported`、`trajectory.open_failed`(现状全量)。

### 命令面错误分型(§8.2)

`/sessions`、`/resume`、`/archive`、`/delete`、`/export` 的报错必须能映射到
上表之一;映射不上的现造新码,不拿自由文本当错误。

## 九、中英字段名对照(冻结,中英同步文档用)

| 英文键 | 中文 |
|---|---|
| workspace / workspace_key | 工作区 / 工作区钥匙 |
| identity_kind / identity_root / checkout_root / git_common_dir / launch_cwd | 识别来源 / 身份规范根 / 检出根 / Git 公共目录 / 启动工作目录 |
| display_name | 显示名 |
| session_id / run_id / agent_run_id / event_id | 会话号 / 运行号 / 子代理运行号 / 事件号 |
| main.jsonl / subagents | 主账 / 子账 |
| start_reason / status / terminal_event_hash | 开场原因 / 状态 / 终态指纹 |
| legacy_import / legacy_partial / unavailable_legacy | 旧档迁入 / 部分导入 / 子账不可得(旧档) |
| training_policy(exclude) | 训练策略(排除) |
| memory_level(project / user) | 记忆层级(项目 / 全局) |
| memory_id / memory_schema / content_sha256 / snapshot_ref | 主题号 / 主题格式 / 正文指纹 / 快照引用 |
| injected_bytes / source_evidence_refs | 注入字节数 / 来源证据引用 |
| intent / progress / result / operation_id | 意图 / 进度 / 结果 / 操作号 |
| source_sha256 / outcome / already_imported / missing | 源指纹 / 单件结果 / 已导入(幂等) / 缺口 |
| tombstone / lifecycle / catalog / recall-traces | 墓碑 / 生命周期账 / 目录册 / 召回踪迹 |

界面用语:全局层叫"全局记忆";schema 一律 `level=user`。两套词不混用
(单子 §8.3)。

## 十、合同实现锚点(后续波次)

- 常量:`src/workspace/storage_contracts.hpp`(P0-0 已落,纯头,不接线)。
- resolver:`src/workspace/identity.*`(P0-1 新建,吃 §4.2 四级裁决)。
- manifest 读写:`src/workspace/manifest.*`(P0-1,原子写沿用
  trajectory/directory.cpp 的 WriteTextFileAtomic 形制)。
- 迁移器:`src/workspace/storage_migrator.*` + `tools/legacy-storage-migrator/`
  (P0-5;生产二进制外)。
- 夹具:`tests/fixtures/workspace/`(P0-0 已落,见该处 README)。
