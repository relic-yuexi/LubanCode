# word-count-rs — process 插件示例(Rust)

Rust 走 process 插件的两条正路之一:编成独立可执行文件(另一条是
`cdylib` 导出 C ABI 走 native 插件,见 examples/plugins/hello_plugin)。
独立 executable 往往更省心:没有跨 CRT free、没有宿主 ABI,崩了也不带倒
主进程——单子「先答五个问题」五。

## 构建

```bash
cargo build --release
# 产物:target/release/word_count_tool(.exe)
```

## 安装

把 `plugin.json` 与可执行文件放进 `~/.lubancode/plugins/word-count-rs/`,
目录里保持 `target/release/word_count_tool` 的相对结构(plugin.json 的
command 用了 `${plugin_dir}` 占位符);或把 command 改成产物绝对路径。
重启 LubanCode,`/plugins` 应见 `word-count-rs: 1 个工具`。

## 要点

- 用户机器**不需要装 Rust**:可执行文件自带 runtime。
- `language: rust` 只给 `/plugins` 与 doctor 展示用,不进模型 prompt,
  也不影响分派——分派只看 `runtime.kind=process + command/args`。
- 构建是作者的事,运行时不 cargo build(执行与构建分家)。
- 想要零文件依赖的单文件分发,把 target/release 下的产物连同
  plugin.json 拷成同目录两枚文件、command 写 `${plugin_dir}/word_count_tool`
  也行(Windows 下记得带 .exe)。
