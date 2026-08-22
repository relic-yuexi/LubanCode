# 工具文案(模型可见,按语言分档)

这个目录是**发给模型看的工具文字**的源码:工具描述(`Tool::description()`)与
schema 参数说明。每工具一个 .md,每个语言一个子目录,构建时由
`cmake/embed_tool_text.cmake` 嵌进 `<build>/generated/embedded_tool_text.hpp`,
运行时由 `src/tools/tool_text.cpp` 的查表模块按当前会话语言取出。
改任何 .md,重新构建即生效,不用重跑 cmake configure。

这里**不放**界面上给人看的文案——那套走 `src/cli/i18n.cpp`,两本账目录分开。
系统提示模块(core/features/platforms)也不在这里,在上一层的 `.md` 与
`agent/prompt_assembler.cpp`。

## 文件格式

```text
src/prompts/tools/
  zh-CN/read_file.md     内置简体中文(默认)
  en/read_file.md        内置英文
  <语言码>/<工具名>.md   其他语言:机制留口子,内容谁要谁自己加
```

每个文件内部按 `## 节名` 分键:

- `## description` —— 工具描述,模型在工具表里看到的那段话;
- `## param.<参数路径>` —— schema 里该参数的 description,参数名与
  C++ 里 `properties["参数名"]` 一致。

节体到下一个 `## ` 或文件尾,前后空白剥掉后嵌入。`## ` 之前的内容忽略。
节体里别出现 `)LUBAN_TT"` 这串字符(raw string 定界符),嵌入脚本查到直接报错。

## 查表与回退

工具 C++ 代码里只剩键名与兜底:

```cpp
std::string ReadFileTool::description() const {
    return ToolText("read_file", "description",
                    "读取文件内容……");  // 兜底 = C++ 原文案,迁移期保底
}
```

回退链:当前语言(cli 的语言选择链:`LUBANCODE_LANG` / config.language →
内置 zh-CN/en → 外部语言包)→ zh-CN → 兜底字串。语言切换即时生效
(每次调用现查)。

## 迁移状态

工具逐批搬进来,一批一枚 commit。已有档案:

- read_file、write_file(基建批 + 文件工具第一批的两枚试点)

其余工具的描述仍在各自 .cpp 里,后续批次照单子
(`todos/工具描述与模型可见文案抽离C++按语言分档.todo`)搬。
