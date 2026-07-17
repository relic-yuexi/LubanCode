# LuBan Prompt Stack

`system_prompt.md` is generated output. Do not edit it by hand.

The source is split into four layers:

1. `core/`: stable instructions shared by every deployment.
2. `features/`: rules that apply only when a capability is enabled.
3. `platforms/`: provider or runtime-specific constraints.
4. `models/<platform>/<model>.md`: constraints verified for one model only.

Build a prompt with `assemble.ps1`. The script always includes every core
module and one platform module. Features, model instructions, and runtime
context are opt-in.

```powershell
.\src\prompts\assemble.ps1 -Platform local -Features files,skills `
  -Workspace D:\lubancode -CurrentDate 2026-07-18 -TimeZone Asia/Shanghai
```

Runtime values are data, not source files. Pass the current date, time zone,
workspace, enabled capabilities, and any supplied memory when building the
prompt. Tool schemas should travel through the API's native tool mechanism,
not be copied into this prompt.

When a provider has a real, tested difference, put it in
`platforms/<provider>.md`. When the difference belongs to one model, put it in
`models/<provider>/<model>.md`. Do not put product names, model names, paths,
dates, or tool inventories in `core/`.
