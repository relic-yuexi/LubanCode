# 项目指令

[文档首页](../../README.md) · [命令手册](../../reference/commands.md) · [架构说明](../../architecture/README.md) · [提示词模块](../../../src/prompts/README.md)

`AGENTS.md` 写仓库规矩。它告诉代理项目怎么搭、命令怎么跑、哪些边界不能碰。它不是聊天记录，也不是答话口吻。

主代理和子代理都读这套指令。新会话启动时读一次；`/init` 新建或发现现有文件后，会当场重载，不必 `/clear`，也不必重启。

## 1. 一分钟上手

进入仓库，执行：

```text
/init
```

随后打开生成的 `AGENTS.md`，把三件事写准：

1. 源码、测试、生成物各放哪儿。
2. 构建、格式化、测试该跑什么命令。
3. 哪些文件不能改，哪些改动必须补测试。

写完重新执行 `/init`。程序见文件已在，不会覆盖，只会重载。

## 2. `/init` 写到哪里

LubanCode 从当前目录往上找 `.git`：

- 找到 Git 根，就在根目录写 `AGENTS.md`。
- 找不到，就把当前目录当项目根。
- 根目录若已有非空 `AGENTS.override.md`，优先认它。
- 否则若已有 `AGENTS.md`，也不覆盖。

脚手架会查看根目录文件，猜一套起步命令：

| 标志文件 | 生成的命令 |
| --- | --- |
| `CMakeLists.txt` | `cmake -S . -B build`、`cmake --build build`、`ctest --test-dir build --output-on-failure` |
| `package.json` | `npm install`、`npm run build`、`npm test` |
| `Cargo.toml` | `cargo build`、`cargo test` |
| `go.mod` | `go build ./...`、`go test ./...` |
| `pyproject.toml` 或 `pytest.ini` | `python -m pip install -e .`、`python -m pytest` |
| `Makefile` | `make`、`make test` |

没认出项目类型时，脚手架只留待填说明，不凭空编命令。生成结果是一张底稿，仍要由项目维护者校准。

## 3. 查找顺序

程序从项目根一路走到当前工作目录。每层只取一份：

1. 非空 `AGENTS.override.md`。
2. 若没有非空 override，再取非空 `AGENTS.md`。

根目录先拼，近处目录后拼。越靠近当前目录，越能补充或压过上层规矩。

```text
repo/
  AGENTS.md                 # 全仓通则
  src/
    AGENTS.override.md      # src 内采用这份，不读同层 AGENTS.md
    parser/
      AGENTS.md             # parser 再添细则
```

若当前目录是 `repo/src/parser`，最终顺序如下：

```text
repo/AGENTS.md
repo/src/AGENTS.override.md
repo/src/parser/AGENTS.md
```

`override` 只压过同层 `AGENTS.md`，不会抹掉父目录指令。空文件跳过。路径会写进拼装后的标题，模型知道每段从哪儿来。

## 4. 32 KiB 上限

所有项目指令正文与来源标题合计最多 32 KiB。达到上限时，程序沿 UTF-8 字符边界截断，免得切坏中文；后续文件不再读入。

这有个实在后果：根目录若写成万言书，近处模块规矩可能挤不进来。通则写短，细则下沉。大段背景材料放普通文档，再从 `AGENTS.md` 指明何时去读。

## 5. 推荐结构

```markdown
# Repository Guidelines

## Project Layout
- Product code lives in `src/`; tests mirror it under `tests/`.
- Do not edit generated files under `build/`.

## Build and Test
- Configure: `cmake -S . -B build`
- Build: `cmake --build build --config Release`
- Test: `ctest --test-dir build -C Release --output-on-failure`

## Working Agreements
- Keep protocol parsing in `src/api/`.
- Preserve unrelated changes in a dirty worktree.
- Add a regression test for every user-visible bug fix.
- Report checks that could not be run.
```

一条规矩最好能回答“何时、在哪儿、做什么”。例如“改 SSE 时补 `tests/test_*_events.cpp`”就比“确保高质量”有用。

## 6. 分层写法

根目录放全仓通则：

- 构建与测试入口。
- 代码生成规则。
- 安全边界。
- 提交与发布规矩。

模块目录放局部细则：

- 该模块的抽象边界。
- 专属测试命令。
- 文件命名与兼容约束。
- 必须阅读的设计文档。

临时迁移期间，可用 `AGENTS.override.md` 盖住同层旧规则。迁移完就删，免得后来的人只见新规，不知为何压住旧规。

## 7. 适合写什么

适合写：

- 可直接执行的命令。
- 路径与模块归属。
- 测试门槛。
- 用户已有改动如何保留。
- 生成文件、密钥、迁移数据等禁区。
- 格式化、兼容性、文档同步要求。

不适合写：

- API key、令牌、内网口令。
- 一次会话才有用的临时安排。
- 长篇产品介绍。
- 与仓库无关的个人偏好。
- 互相打架、又不说明作用域的命令。

`AGENTS.md` 通常会进 Git。凡写进去的字，都按公开给项目协作者看待。

## 8. 与其他提示层的关系

| 文件 | 职分 |
| --- | --- |
| `~/.lubancode/system_prompt.md` 或自定 system prompt | 代理身份与总工作方式 |
| `~/.lubancode/SOUL.md` | 口吻、措辞、个人偏好 |
| `AGENTS.md` | 仓库规矩 |
| `SKILL.md` | 某类任务的具体流程与资源 |

项目指令不能越过程序权限。它可以要求模型调用工具，却不能绕开确认、路径校验、命令黑名单和输出上限。

## 9. 当前会话何时生效

- 新启动会话：启动时加载。
- `/init`：无论新建还是发现现有文件，都会重建主代理提示。
- `/init` 之后调用子代理：子代理拿到新指令。
- 在外部编辑器里直接改文件：当前会话不会自动监听；重新执行 `/init` 或重启。

已经写进历史的旧系统提示不会逐条改写。后续请求使用重建后的提示。

## 10. 排错

**子目录指令没生效**

先看当前工作目录。程序只从项目根走到 cwd，不会扫描 cwd 下面尚未进入的目录。

**同层 `AGENTS.md` 没读**

看看同层是否有非空 `AGENTS.override.md`。有它便只取 override。

**`/init` 没覆盖旧文件**

这是刻意的。项目规矩不能由脚手架悄悄洗掉。手工编辑旧文件，再跑 `/init` 重载。

**近处规矩不见了**

多半撞上 32 KiB。删短根目录长文，把背景移到 `docs/`。

**找错项目根**

确认上级目录哪一层有 `.git`。Git worktree 的 `.git` 可以是文件，程序同样按存在处理。
