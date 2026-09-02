# 旧数据迁移(storage v2)

[文档首页](../README.md) · [排错手册](troubleshooting.md) · [workspace 存储设计](../development/workspace-storage-v2/)

LubanCode 0.26.x 起会话与项目记忆改存统一的 workspace 树(`~/.lubancode/workspaces/`)。旧格式的用户数据不会自动搬迁,新运行时也不再去读它——要接续旧会话与旧项目记忆,跑一次下面的迁移器。这页讲迁移怎么做、怎么核对、什么版本之后旧档不再被支持。

## 1. 谁需要迁移

你的 `~/.lubancode/` 里若有这些旧格式数据,便需要迁:

| 旧数据 | 位置 | 迁到 |
| --- | --- | --- |
| 旧会话档 | `~/.lubancode/sessions/*.jsonl`(含 `sessions/archive/`) | `workspaces/<key>/sessions/<id>/`(typed trajectory 事件流) |
| 旧项目记忆 | `~/.lubancode/projects/<key>/memory/` | `workspaces/<key>/memory/` |
| 记忆候选箱 | `~/.lubancode/projects/<key>/memory-candidates/` | `workspaces/<key>/memory/memory-candidates/` |
| 全局记忆 | `~/.lubancode/memory/user/` | 不搬,路径不动(只升 schema 与权限合同) |

没有上述目录,或目录是空的,什么都不用做——新版本从空 home 起照常工作。

## 2. 三步走:plan → run → status

```powershell
# 1) 只列计划,不动任何文件:每场旧会话迁去哪个 workspace、旧项目库映射到哪
lubancode migrate-storage plan

# 2) 执行迁移(可反复跑,中断了再跑同一只会自动续)
lubancode migrate-storage run

# 3) 查账:还有哪些旧档没迁、哪些旧项目算不出目标
lubancode migrate-storage status
```

规矩:

- **旧源一字不动**。迁移器只读旧档、只写新账;默认不删除任何旧文件。
- **幂等**。同一份源(按 SHA-256 认)迁过一次,再跑只会对账回指,不会造出第二份。
- **可中断续跑**。迁移中途断电、杀进程,重跑 `run` 会从最近未完成的 operation 续上;写到一半的目标场会删掉重建,不留半截。
- **迁完自验**。每场导入都过 schema 校验与全量回放(verify + replay),不过的场标 `failed` 并给出稳定错误码,不冒充成功。

### 2.1 旧项目算不出目标怎么办

`plan` 或 `status` 里出现 `unmappable` 的旧项目,是 `projects/<key>/` 下没有 `project.json`、也推不出它对应哪个目录。用 `--project-root` 显式指认(可多枚):

```powershell
lubancode migrate-storage run --project-root D:\work\my-repo --project-root D:\work\other-repo
```

### 2.2 删除旧源(可选,慎用)

确认新账都已落好、想清掉旧档时:

```powershell
lubancode migrate-storage run --delete-source --yes
```

`--delete-source` 必须配 `--yes` 二次确认;只删**已 committed 且当下复验通过**的会话源档,任何一环不合即整批不删。旧项目记忆库不在此列——它默认原样保留。拿不准就先不删:留着旧档不影响新版本运行。

## 3. 迁移回执

每只迁移 operation 在 `~/.lubancode/migrations/storage-v2/<operation-id>/` 留三份账:

| 文件 | 内容 |
| --- | --- |
| `intent.json` | 迁移前列的源清单:每件路径、字节数、SHA-256 |
| `progress.json` | 逐文件进度(phase/done/total),中断续跑的依据 |
| `result.json` | 终局回执:逐件结果、目标 session id、terminal hash、缺口清单。**只有它落盘才算 committed** |

回执里值得看的字段:

- `counts.imported / already_imported / skipped_unreadable / failed`:四类归宿的件数。
- `items[].legacy_partial`:这场旧会话有不可完全还原的边界(如 usage 账、子代理细账),新场标了 `legacy_partial`、训练策略 `exclude`。
- `items[].missing`:逐条缺口,照实列账,不静默丢。
- `memory_projects[]`:旧项目记忆逐主题的 source/target hash 对照。

## 4. 迁移的边界(如实说)

旧格式本身记不下的信息,迁移后不会有:

- **usage 账**:旧消息行不带 token 计数,迁不过去。
- **子代理细账**:旧主账只有子代理的最终回话,新场标 `subagent_detail=unavailable_legacy`,不伪造子 Journal。
- **工具执行细账**:时长、退出码等按保守值导入并列入 `missing`;`tool_trace_v1` 行若在,四档结论会融进终态。
- **compact 裁剪语义**:新账只记 `compact.applied` 边界,不重写 Journal。

## 5. 独立迁移工具与版本边界

迁移引擎隔离在独立构建目标里,不进生产运行时。命令面有两个入口,引擎同源:

- `lubancode migrate-storage ...`:主程序子命令,过渡期内置。
- `legacy-storage-migrator ...`:独立可执行文件(构建树 `tools/legacy-storage-migrator/`),参数同上,另有 `--home <dir>` 指定主目录。收官发行把主程序这条入口摘除后,迁移一律用独立工具。

版本边界:

- **0.26.x**:旧档还在原处即被迁移器识别;`migrate-storage` 内置于主程序。
- **最后兼容版本**:旧格式读取与迁移能力保留到 **0.26 系列收官**。此后的发行不再随包携带旧格式解析;尚未迁移的旧档需要退回最后兼容版本先迁,或永久留在旧目录(新版本不读也不删)。

一句话:要接旧会话,趁 0.26 系列跑一次 `migrate-storage run`;跑完 `status` 见零 pending,旧账就算接清了。
