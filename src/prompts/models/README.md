# 模型模块(故意留空)

模型专属指令不放这儿。模型身份、模型专属提示走 `~/.lubancode/models.json` 目录里
`base_instructions` 字段,随当前模型注入系统提示(`src/agent/prompts.hpp` 的
`WithModelInstructions`),`/model` 切换即换。

要给某个模型写专属提示,改 models.json,别往这个目录添文件——本目录的 .md
不参与编译期嵌入(嵌入脚本只认 core/、features/、platforms/ 三处)。
