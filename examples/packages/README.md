# Package 示例

整箱分发的示例:每只目录一只包,根上有 `package.yaml`(schema 1),
组件按六类标准目录摆。契约全文见 `docs/reference/packages.md`;
完整包的活样例另见 `tests/fixtures/packages/full-stack/`。

| 包 | 档位 | 装什么 |
| --- | --- | --- |
| [browser-agent/](browser-agent/README.md) | content-only | 浏览器操作纪律 Skill 加练习页;零代码组件,放进目录即用 |
| [gui-agent/](gui-agent/README.md) | code-bearing | Windows 桌面 process 插件(十件工具)加配套 Skill;代码组件默认只被发现不挂载,执行须过信任门 |

## 验一只包(不动配置)

```text
lubancode --package-dir <本目录>
/package list
/package show <包 id>
/package doctor <包 id>
```

`--package-dir` 是开发层,优先级最高。装法、信任门、工具 wire 名,
各包 README 里各有交代。
