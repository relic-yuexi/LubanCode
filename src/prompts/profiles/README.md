# profiles/ — 内置 Prompt Profile

每个子目录一个 Profile(名字须小写 kebab-case),目录里按 `core/`、`features/`、
`platforms/` 摆**稀疏覆盖**——只放要点名的模块文件,没点名的模块原样走默认。

```text
profiles/browser-tester/core/10-identity.md    只换身份模块
profiles/browser-tester/features/web.md        只换联网方针
```

覆盖与拼装次序见 `docs/reference/agents.md` §6:

```text
内置 default 模块 -> 用户全局 default 覆盖 -> 内置选中 Profile 覆盖
-> 用户选中 Profile 覆盖 -> 项目选中 Profile 覆盖
```

用户层与项目层的位置:

```text
~/.lubancode/prompts/profiles/<名>/<相对路径>
<项目根>/.lubancode/prompts/profiles/<名>/<相对路径>
```

`modes/` 不可覆盖(宿主策略);模型专属指令、工具 schema、权限门都不归 Profile 管。
没有 `profiles/default/` 也能跑——`src/prompts/{core,features,platforms}` 本身就是
隐式 default。

Agent YAML 里用 `prompt.profile: <名>` 点名;哪段提示词从哪层来,`/agent inspect`
的来源账本(PromptSourceLedger)会逐段列账。
