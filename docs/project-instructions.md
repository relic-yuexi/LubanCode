# 项目指令

[文档首页](README.md) · [终端交互](terminal-ui.md) · [提示词模块](../src/prompts/README.md)

`AGENTS.md` 写仓库规矩。它不是聊天记录，也不是答话风格。LubanCode 每次建会话都把它并入系统提示，让主代理与子代理知道目录布局、构建命令、测试门槛和改动边界。

## `/init`

进入项目目录，输入：

```text
/init
```

LubanCode 先找 Git 根。找得到，便在根目录写 `AGENTS.md`；找不到，就写在当前目录。脚手架会按仓库文件识别 CMake、npm、Cargo、Go、Python 或 Make，并填入相应构建测试命令。

已有 `AGENTS.md` 或 `AGENTS.override.md` 时，`/init` 不覆盖，只重新载入。新文件也在当前会话立刻生效，不必 `/clear`，不必重启。

## 查找顺序

LubanCode 从 Git 根走到当前工作目录。每一层按这套次序找：

1. 非空的 `AGENTS.override.md`
2. 非空的 `AGENTS.md`

同一层只取一份。根目录内容先放，子目录内容后放；越靠近当前目录，话语权越大。空文件跳过。所有文件正文合计最多 32 KiB，超出部分截去。

```text
repo/AGENTS.md
repo/src/AGENTS.override.md
repo/src/parser/AGENTS.md
```

若当前目录在 `repo/src/parser`，三层依次读入。`src` 那层有 override，便不再读同层 `AGENTS.md`。

## 怎么写

写能落地的规矩。把命令、路径、边界说清。少写空话。

```markdown
# Repository Guidelines

## Build and Test
- Build: `cmake --preset release`
- Test: `ctest --test-dir build/release -C Release --output-on-failure`

## Working Agreements
- Keep protocol parsing in `src/api/`.
- Add a regression test for every user-visible bug fix.
- Do not edit generated files under `build/`.
```

团队通用规矩放根目录。某个模块另有讲究，就在那层加 `AGENTS.md`。临时要压过同层旧规矩，再用 `AGENTS.override.md`。
