# broken:故意损坏的诊断夹具

五个子目录,五只独立坏包。每个子目录单独丢进 packages 目录,`/package doctor` 应逐条报出下表的错。
**别把 `broken/` 整个丢进去**——它自己没有清单,那只验证了 `manifest_missing`,查不出别的。

错名与 `docs/packages.md` 第 10 节的诊断表一一对应。

## missing-manifest/ —— 缺清单

| 位置 | 错 | 应报 |
| --- | --- | --- |
| 包根 | 没有 `package.yaml`,`skills/packer/SKILL.md` 本身无辜 | `manifest_missing`,整包 invalid,组件一件不挂 |

## invalid-manifest/ —— 清单写坏

| 位置 | 错 | 应报 |
| --- | --- | --- |
| `schema` | 写 `2`。首版只认 `1` | `manifest_schema_unsupported`,拒载并提示版本 |
| `id` | `Moontide.Bad_Package`:大写、下划线,段不合 kebab-case | `id_invalid` |
| `version` | `"1.0"`,缺 PATCH,非 SemVer | `version_not_semver` |
| `permissions` | 未知字段。清单不装能力,`network` 与 `tools_allowlist` 都是越权 | `unknown_field`(两条子错) |

组件 `skills/packer/SKILL.md` 是好的——清单有罪,整包照样 invalid。

## path-escape/ —— 越界引用

清单合法,组件往外跑:

| 位置 | 错 | 应报 |
| --- | --- | --- |
| `mcp/leaky/mcp.yaml` | args 写 `${package_dir}/../../shared/server.js`,规范化后逃出包根 | `path_escape`,该组件拒载,整包 invalid |
| `workflows/checkout/workflow.yaml` | `prompt:` 写 `prompts/../../shared/report.md`,越出 workflow 目录与包根 | `path_escape`,指到行 |

## bad-names/ —— 命名不合规

清单合法,六处名不正经:

| 位置 | 错 | 应报 |
| --- | --- | --- |
| `skill/` | 顶层目录少个 s,近似名,不是标准目录 | `near_miss_directory`,明报"是否想写 skills/",不当普通未知目录 |
| `agents/Browser-Tester.yaml` | 文件名大写连字符,不合 kebab-case | `name_invalid` |
| 同文件 `name: browser tester` | 名字带空格 | `id_invalid` |
| `skills/audit/SKILL.md` | frontmatter `name: release_check`:下划线不合规,且与目录 `audit` 不符 | `name_invalid` + `name_mismatch` |
| `plugins/dom.analyzer/plugin.json` | id 含点。点号是命名空间分隔符,local id 不可用;现有 plugin 规矩 `[A-Za-z0-9_-]` 也不收 | `id_invalid` |
| `mcp/db/mcp.yaml` | `id: database` 与目录名 `db` 不一致 | `name_mismatch` |

## code-failure/ —— 静态全好,起不来(阶段 5 的坏法)

前四只坏在解析期,医生一眼看穿;这只坏在运行期——静态账全绿,批了信任也挂不上:

| 位置 | 错 | 应报 |
| --- | --- | --- |
| `plugins/dies-loud/runner.py` | 启动即退非零(无害夹具,一行协议不回) | 挂载事务探针 `tool_exit_non_zero`,整包回滚:同包的 `count-words` 插件与 `ledger` MCP 一件不进 ToolRegistry,已起进程全停 |
| 其余件 | `count-words`、`ledger` 与 `code-stack` 同款,全是好的 | 无辜,连坐 |
