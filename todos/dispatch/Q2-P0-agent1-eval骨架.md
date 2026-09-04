# 派工单｜Q2-P0-agent1｜eval 目录骨架与三组分组

> 母单：`todos/工具与上下文治理量化评测_Q2.todo`（第 P0 章）
> 派活形态：one_shot（单只一次跑完，分批会被打断打断打断打断打打断）
> 钥匙清单：本单独占 `tests/eval/CMakeLists.txt`、`tests/eval/README.md`、`tests/eval/<实验名>/` 三个子目录的创建权。其他 subagent 写的是这三个子目录内部的 .hpp/.cpp/.py，**你不动他们的文件**

## 一、做什么

把 `tests/eval/` 这个评测分组从无变有。CMake 默认不编它，CTest 默认不跑它；opt-in 才入。

四步：

1. 建目录：

```text
   tests/eval/
   ├── CMakeLists.txt
   ├── README.md
   ├── support/                  ← 给 agent-2 的 fake backend 与 agent-3 的 collect.py 共享
   │   └── (空)
   ├── tool_search_threshold/    ← 实验 A 落点（agent-3 在这写 collect.py）
   ├── compaction_benefit/       ← 实验 B 落点
   └── subagent_failure/         ← 实验 C 落点
```

2. 写 `tests/eval/CMakeLists.txt`：

```cmake
# opt-in 评测分组——默认不编、不跑。
# 单独开：cmake -DLUBANCODE_EVAL=ON ..  &&  ctest -L eval
add_library(lubancode_eval_support INTERFACE)
target_include_directories(lubancode_eval_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# 子目录按实验落 CMakeLists，继承 INTERFACE 库
add_subdirectory(support)        # 留给 agent-2 写 fake_backend.cpp 时挂
add_subdirectory(tool_search_threshold)
add_subdirectory(compaction_benefit)
add_subdirectory(subagent_failure)
```

3. 在 `tests/CMakeLists.txt` 末尾追加：

```cmake
option(LUBANCODE_EVAL "Enable eval harnesses (off by default)" OFF)
if(LUBANCODE_EVAL)
  add_subdirectory(eval)
endif()
```

4. 写 `tests/eval/README.md`：开组方式（`-DLUBANCODE_EVAL=ON`）、各实验落点路径、每实验 README 与 collect.py 出入口约定。

## 二、验收

- `cmake -B build -DLUBANCODE_EVAL=ON ..` 通过（默认 Release 工具链与现有 doctest 不冲突）。
- `cmake --build build` 通过（即使三个实验子目录还是空，INTERFACE 库也不会炸）。
- `ctest -L eval` 在三个实验子目录为空时返回 "No tests"，不报错。
- 不开 `-DLUBANCODE_EVAL=ON` 时，`tests/eval/` 一字不被读到。

## 三、不做什么

- 不动现有 `tests/CMakeLists.txt` 任何已注册分组。
- 不在 `tests/eval/<实验名>/` 下写 .hpp/.cpp/.py——那是 agent-2 / agent-3 的活。
- 不引第三方依赖（nlohmann/json、doctest 已全仓可用）。
- 不写 .todo / 不改 .todo 母单。
- 不 push。

## 四、Worktree 操作

- 从 `origin/main` 开：`git fetch origin && git worktree add ../wt-q2-p0-agent1 -b eval/q2-p0-agent1 origin/main`
- 提交落本 worktree，不推。
- 完工后停；不 merge main、不合 main、不 push。
- 派工指令里写明："先 cd 到 worktree 再开工，所有命令在 worktree 里跑"。

## 五、报告路径

- 完工落点：`tests/eval/CMakeLists.txt`、`tests/eval/README.md`、`tests/CMakeLists.txt`（追加三行）、三个空子目录
- 报告落 `todos/dispatch/Q2-P0-agent1.report.md`（一句话：做了什么、踩了什么、还差什么）
- 日志重定向：`cmake -B build 2>&1 | tee build/cmake.log`