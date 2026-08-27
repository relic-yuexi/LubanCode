# 插件示例

四条路各一枚,照抄改名字就能起步。完整写法见
[扩展 LubanCode](../../docs/features/extensions/README.md)。

| 示例 | runtime | 语言 | 说明 |
| --- | --- | --- | --- |
| [word_count.lua](word_count.lua) | embedded-lua(legacy) | Lua | 一文件一工具:数词数 |
| [local_math/](local_math/) | process | Python | add 工具 + 协议 v1 runner + 自测命令 |
| [word_count_rs/](word_count_rs/) | process | Rust | cargo 出可执行文件当工具,用户机器零依赖 |
| [local_math_c/](local_math_c/) | process | C | gcc/cl 出可执行文件,示例级 JSON 解析 |
| [hello_plugin/](hello_plugin/) | native-library(ABI v2) | C | 三平台 .dll/.so/.dylib + host allocator buffer 契约 |
| [agents/gui-agent/](../agents/gui-agent/) | process | Python | 较完整案例:十件 GUI 工具(UIA 快照/截图/点击/输入),坐标合同、dry-run、stale 拦截、教学夹具与 E2E 全带 |

起步最快的一条:`lubancode plugin init python my-tool` 生成三件套
(plugin.json + runner.py + test_runner.py),本地 `python test_runner.py`
先自测,再改 HANDLERS 换成自己的工具。
