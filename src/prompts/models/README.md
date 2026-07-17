# Model Modules

Add a model module only after testing a real difference. Its path is
`models/<platform>/<model>.md`.

Examples:

```text
models/openai/gpt-5.md
models/minimax/MiniMax-M3.md
models/qwen/qwen3-coder.md
```

Keep each module narrow: output quirks, verified tool-call limits, or a
model-specific workaround. Do not repeat core safety, general style, or
provider-wide rules.
