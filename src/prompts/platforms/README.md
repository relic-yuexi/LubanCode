# Platform Modules

Each platform file contains only verified, durable constraints of that runtime.
It must not declare the current date, workspace, enabled tools, account state,
or model availability; those are runtime context.

Use one platform file per provider or execution environment:

```text
local.md       LuBan's native command-line runtime
openai.md      OpenAI-backed runtime
anthropic.md   Anthropic-backed runtime
minimax.md     MiniMax-backed runtime
deepseek.md    DeepSeek-backed runtime
qwen.md        Qwen-backed runtime
zhipu.md       Zhipu-backed runtime
kimi.md        Kimi-backed runtime
```

Provider-wide rules belong here. A limitation or instruction that applies only
to one model belongs in `models/<platform>/<model>.md`.
