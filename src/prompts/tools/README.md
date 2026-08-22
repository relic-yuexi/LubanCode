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
- edit_file、search(文件工具批余量)
- run_command、background_output、stop_background;批3-5(0.26.21):agent、agent_message(含 persona.general/persona.explore)、ask_user、todo_write、lsp、context_search、context_read(命令族批)
- 清底批(全部迁完):web_search、web_fetch、tool_search、skill、list_sessions、send_session_message、worktree、memory_save、programmatic_tool_calling

run_command 的描述与 command/shell 两个参数的平台分档文案,档里单列
`(POSIX)` 节(如 `## description (POSIX)`),cpp 侧键名随 `#ifdef` 走;
Windows 用主键,POSIX 用 `(POSIX)` 键。timeout_ms / run_in_background /
cwd 三参数两平台一字不差,只入主键,不分档。

静态工具已全部迁完。不迁的只剩动态描述:lua_tool、plugin_tool/plugin_loader、
mcp_tool——外挂自带描述,进不了档。
