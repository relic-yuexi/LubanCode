<p align="center">
  <img src="docs/assets/lubancode-banner.png" alt="LubanCode" width="100%">
</p>

<h1 align="center">LubanCode</h1>

<p align="center"><strong>A lightweight local Agent Harness written in C++23. It reads code, edits files, runs commands, and delegates work with the model and endpoint you choose.</strong></p>

<p align="center">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

<p align="center">
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml?query=branch%3Amain+event%3Apush"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml/badge.svg?branch=main&event=push" alt="main branch CI"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-3D6DCC" alt="Apache-2.0"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-444444" alt="Windows, Linux and macOS">
</p>

LubanCode is not a thin shell around one model vendor. It connects a model to a local execution harness: inspect files, search a repository, edit code, run commands, delegate work, and call MCP or LSP tools. The harness owns permissions, diffs, token accounting, cancellation, and the record left behind.

It speaks Anthropic Messages, OpenAI Responses, OpenAI-compatible Chat Completions, and Gemini Generate Content. Its built-in catalog carries presets for 68 providers, while custom and local vLLM-compatible endpoints remain first-class. You bring the model; LubanCode runs the work.

The name comes from Lu Ban, the traditional Chinese master craftsman. The idea is simple: measure first, cut second, and always show the work.

**[Run your first task in three minutes](docs/getting-started/quickstart.md)** · **[Read the C++ trade-offs and tool comparison](docs/getting-started/why-lubancode.md)**

> [`src/app/version.hpp`](src/app/version.hpp) is the source of truth for the version. Code changes on `main` build and run the full test suite on Windows, Ubuntu, and macOS; the CI badge above reports the current result.

## Real terminal captures

These frames came from the Windows Release binary connected to a local test provider. No mockup and no real API key were used.

<p align="center">
  <img src="docs/assets/screenshots/agent-queue.png" alt="LubanCode agent dock, busy composer, queued input, and status bar" width="100%">
</p>
<p align="center"><sub>Keep typing while the main agent works; sub-agents, queued input, and status stay on one screen.</sub></p>

<p align="center">
  <img src="docs/assets/screenshots/markdown-session.png" alt="LubanCode rendering a Markdown heading, table, and code block in the terminal" width="100%">
</p>
<p align="center"><sub>Markdown headings, tables, and code blocks render directly in the terminal.</sub></p>

## The short version

Start `lubancode` inside a repository and describe a task. It loads project instructions, assembles model context and tools, checks permissions before actions, manages processes and cancellation, and records messages, tool calls, usage, and trajectory data when the turn closes.

```text
task -> project instructions -> model -> tools/agents/workflows -> verification -> session + trajectory
```

Use it for bugs, tests, and diff review. Go deeper and it can run typed workflows, serve a headless app-server, capture agent trajectories, and diagnose local model compatibility. It is both a coding CLI and an Agent Harness you can inspect, extend, and study.

## What lightweight means here

LubanCode keeps installation and idle machinery small even though the feature set is broad. Releases use a native program and require neither Node.js nor Python. The archive also carries `rg`, so repository search works immediately.

An interactive session starts external components as they are needed. LSP servers start on first use and stop after an idle period. When the tool set grows, MCP and plugin tools can remain indexed until the model searches for one. Gateway and channel processes start only through explicit commands, never just because a configuration file exists.

Redirected and piped output drops terminal repainting and uses plain text. In this project, lightweight means few runtime dependencies, little idle machinery, and on-demand loading. The repository has no controlled startup-time or memory comparison across these four CLIs, so the README makes no performance claim.

## The terminal speaks your language

Asking a model to answer in Chinese is easy. Localizing the CLI itself is a different job. LubanCode routes menus, help, approvals, common status lines, and errors through i18n. It detects the system language, includes Simplified Chinese and English, and switches immediately with `/language`. Drop a JSON file into `~/.lubancode/languages/` to add another language without changing or rebuilding the program.

This changes interface text only. It does not rewrite system prompts, tool descriptions, or model replies. The Chinese table is complete; English covers the core keys and falls back to Chinese for gaps. External packs may also begin as partial translations. See [Interface languages](docs/development/i18n.md) for the lookup chain and pack format.

## Why C++

This is not a language contest. LubanCode lives close to the operating system: terminals, streaming I/O, signals, child processes, process trees, and cancellation across Windows, Linux, and macOS. C++ keeps those boundaries inside one native process.

- **Clean delivery.** Releases are native programs. Users do not need Node.js or Python; the archive carries `rg`, official Skills, docs, and licenses.
- **Explicit ownership.** HTTP streams, subprocesses, terminal frames, and cancellation tokens have traceable lifetimes. Long-running agents need clean shutdown and committed records more than a flashy benchmark.
- **Direct platform control.** Windows uses `CreateProcessW`, Job Objects, and UTF-16 paths; POSIX uses `fork/exec`, process groups, and `poll`. Higher layers consume one contract.
- **Embeddable and connectable.** The CLI sits beside an app-server, Lua, MCP, LSP, process plugins, and a C ABI plugin path. Existing C/C++ libraries need not become JavaScript services first.

The trade-off is real: C++ builds are slower, portability work is demanding, and memory safety rests on engineering discipline, tests, and review. TypeScript is usually faster for a small extension. LubanCode chooses C++ to put execution, terminal behavior, and observability on one native foundation. Read [Why LubanCode](docs/getting-started/why-lubancode.md) for the full argument.

## How it differs from Claude Code, Codex CLI, and Pi

All four can read code, edit files, and run commands. Their center of gravity differs.

| Tool | Center of gravity | Best fit |
| --- | --- | --- |
| **Claude Code** | The Claude product ecosystem across terminal, IDE, desktop, web, remote, and team surfaces. | Teams already invested in Claude that want a mature cross-surface product. |
| **Codex CLI** | OpenAI Codex in the local terminal, with native sandboxing and approvals plus connections to Codex desktop, IDE, and cloud surfaces. | ChatGPT/OpenAI users who value the OpenAI ecosystem and its sandbox model. |
| **Pi** | A deliberately small core extended through TypeScript extensions, skills, prompts, themes, and packages. | Hackers who want to reshape a harness quickly in TypeScript. |
| **LubanCode** | A provider-neutral C++ execution harness with replayable, inspectable local records. CLI, workflows, app-server, trajectories, and plugins share one runtime contract. | Multi-protocol or self-hosted model users, harness researchers, and native systems builders. |

LubanCode is not a drop-in replacement for the other three. Claude Code and Codex CLI have broad product ecosystems behind them; Pi is quicker to reshape in TypeScript. LubanCode takes another route: no model-vendor center, and an execution path you can follow from configuration provenance through every tool call.

One smaller difference is built-in interface i18n. LubanCode can follow the system language or switch live with `/language`. A model answering in Chinese is not the same as the CLI localizing its menus, approvals, and errors. The current Claude Code CLI reference, Codex CLI configuration reference, and Pi TUI documentation do not list an equivalent interface-language switch. That is a bounded documentation finding, not a claim that those tools can never support languages other than English.

Comparison scope: 2026-09-04, based on the [Claude Code overview](https://code.claude.com/docs/en/overview), [Claude Code CLI reference](https://code.claude.com/docs/en/cli-reference), [official Codex CLI docs](https://learn.chatgpt.com/docs/codex/cli), [Codex CLI configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference), and [Pi TUI documentation](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/tui.md). The [full comparison](docs/getting-started/why-lubancode.md) records boundaries and selection advice.

## Where LubanCode spends its effort

- **A terminal built for work.** Keep typing while the model runs; queue messages, watch sub-agents, and inspect rendered Markdown, LaTeX, diffs, and full tool output.
- **Separate Law from Soul.** `system_prompt.md` controls behavior; `SOUL.md` controls voice. Swap style without dismantling workflow rules.
- **Repository-scoped memory.** Main and linked worktrees share project identity. `/memory` exposes recall and reviewable writes.
- **More than a transcript.** Sessions store conversation, trajectories store runtime facts, usage stores token accounting, and artifacts hold large content.
- **More than one extension path.** Skills, workflows, MCP, LSP, Lua, process plugins, and C ABI plugins each have an explicit boundary.

## At a glance

| Area | What is included |
| --- | --- |
| **Model access** | Anthropic, Responses, Chat Completions, and Gemini; presets for 68 providers; remembered endpoint switching. |
| **Coding tools** | Read, write, tolerant block editing, file search, foreground and background commands, diff-first approval. |
| **Semantic tools** | LSP definitions, references, symbols and diagnostics; MCP stdio; web search and fetch. |
| **Agent workflow** | Sub-agents, role-based model routes, Plan mode, todo tracking, `ask_user`, `AGENTS.md`, worktrees, and project permissions. |
| **Terminal UX** | Incremental Markdown rendering, animated work status, a persistent queue, collapsed multiline paste, focused views and compact output. |
| **Prompts and memory** | `system_prompt.md` governs behavior, `SOUL.md` governs voice; the main worktree and linked worktrees share one project memory. |
| **Context and sessions** | Sessions store the conversation, trajectories store runtime facts, usage splits token counts, and artifacts hold large content; compaction, resume, and Markdown export. |
| **Interface languages** | System-language detection, built-in Simplified Chinese and English, live `/language` switching, and external JSON language packs. |
| **Extensibility** | Skills, workflows, MCP, LSP, Lua, process plugins, C ABI plugins, hooks, themes, and SOUL. |

## Installation

### Windows: one command

Run this from PowerShell 5.1 or newer. It downloads the latest Windows release, installs it for the current user, and updates the user PATH:

```powershell
irm https://raw.githubusercontent.com/relic-yuexi/LubanCode/main/scripts/install.ps1 | iex
```

The default destination is `%LOCALAPPDATA%\Programs\lubancode`. The bundled `skills/` and `docs/` are installed and upgraded with the executable. No administrator shell is required. Running the installer again performs an in-place upgrade.

For a manual install, download `lubancode-vX.Y.Z-windows-x64.zip` from [Releases](https://github.com/relic-yuexi/LubanCode/releases), extract it, and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

Use the bundled `uninstall.ps1` to remove it.

### Linux / macOS

Download the matching archive from [Releases](https://github.com/relic-yuexi/LubanCode/releases), extract it, enter the extracted directory, and run:

```bash
./install.sh
```

The script uses `~/.local/bin` when it is already on PATH. Otherwise it falls back to `/usr/local/bin` and tells you when `sudo` is needed. Bundled `skills/` and `docs/` are installed together under the matching `share/lubancode/` resource root.

| Platform | Release asset |
| --- | --- |
| Windows x64 | `lubancode-vX.Y.Z-windows-x64.zip` |
| Linux x64 | `lubancode-vX.Y.Z-linux-x64.tar.gz` |
| macOS arm64 | `lubancode-vX.Y.Z-macos-arm64.tar.gz` |

### Build from source

You need CMake 3.21 or newer and a compiler with C++23 support. Dependencies come from a vcpkg manifest when available, with a FetchContent fallback.

Windows:

```powershell
cmake --preset release
cmake --build --preset release
.\build\release\Release\lubancode.exe --version
```

Linux / macOS:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/lubancode --version
```

On Debian or Ubuntu, install the common build prerequisites with:

```bash
sudo apt-get install -y build-essential cmake ninja-build libssl-dev
```

## Quick start

Run `lubancode` without arguments. With no model connection configured, the first screen offers two paths: add a provider now, or skip setup and enter the main screen. Skipping does not write a dummy connection. Run `/provider` or `/provider add` whenever you are ready.

```text
$ lubancode
Getting started with lubancode

No model connection is configured yet.
Add a Provider now, or enter the main screen first.

> Add Provider  - choose from the catalog and enter a key
  Skip for now  - configure later with /provider or /provider add
```

Common entry points:

```bash
# Interactive session
lubancode

# One-shot task with tools enabled
lubancode "Inspect this repository and fix the most important issue"

# Resume the newest session for the current directory
lubancode --continue

# Pipe input into a task
git diff --cached | lubancode "Review this change"
```

For multiple endpoints, run `/provider add`. The 68 presets open in a searchable picker. Type a provider name, select it, and enter the key; the catalog supplies the URL, wire, default model, limits, and provider options. The final item keeps the fully custom wizard. Hand-written configuration remains supported:

```json
{
  "active_provider": "work",
  "providers": [
    {
      "name": "work",
      "wire": "responses",
      "base_url": "https://your-provider.example/v1",
      "key_env": "WORK_MODEL_API_KEY",
      "model": "your-model",
      "context_window": "256k"
    }
  ]
}
```

Save it as `~/.lubancode/config.json`, then set `WORK_MODEL_API_KEY`. A successful `/provider switch work` also writes `active_provider`, so the next launch uses the same endpoint. The detailed [configuration guide](docs/reference/configuration.md) is currently written in Chinese, but all field names and examples are language-neutral.

The catalog source lives in [`catalog/providers.json`](catalog/providers.json), with its schema in [`catalog/providers.schema.json`](catalog/providers.schema.json). The executable embeds a snapshot and caches validated updates under `~/.lubancode/cache/`.

## Project instructions

Run `/init` inside a repository. LubanCode creates `AGENTS.md` at the Git root with a practical scaffold for layout, build, tests, and working rules. Existing instructions are never overwritten. The current main agent and sub-agents reload the file immediately.

At startup, LubanCode walks from the Git root to the working directory. Each directory prefers `AGENTS.override.md`, then `AGENTS.md`; nearer files appear later and take precedence. Empty files are skipped; the combined "separator + source heading + body" is capped at 32 KiB (a fixed ~100-byte outer wrapper is added on top). Writes re-resolve the chain for the target file, so nested `AGENTS.md` files apply automatically, and `/instructions` shows the per-file ledger. See [Project instructions](docs/features/project-instructions/README.md).

## Everyday commands

| Command | Purpose |
| --- | --- |
| `/provider` | Add from the catalog, refresh it, list, switch, edit, or remove endpoints. |
| `/init` | Create and load project-level `AGENTS.md`. |
| `/model` · `/think` | Change model and effort; `/model roles` shows all routes, while `/model <role> <id>` updates one route. |
| `/doctor` | Diagnose a local OpenAI-compatible endpoint: `effort` sends a tiny probe (actual field sent, usage split); `cache` reads server metrics and audits fixed-prefix hit rates. |
| `/context` · `/compact` | Inspect context use (usage of the most recent main-session request, sub-agent tokens not included) and compact conversation history. |
| `/skills` · `/skill` | Manage skills under `~/.lubancode/skills`; run `/skill` bare for install examples. |
| `/mcp` · `/lsp` · `/plugins` | Inspect external tools and language servers. |
| `/tools` · `/todos` | Inspect tool loading state and the current task list. |
| `/plan` | Enter read-only planning mode, inspect first, then review the plan before execution. |
| `/trace` | Inspect the tool lifecycle ledger; `undo_file_edit` can restore an edit when its preimage still matches. |
| `/goal` · `/loop` | Manage persistent goals and scheduled loops when their feature gates are enabled. |
| `/memory` | Manage per-session project memory, synchronous retrieval, and background writes. |
| `/sessions` · `/resume` · `/export` | Pick an older session, replay it, continue, or export it. |
| `/worktree new\|list\|exit` | Work in isolated Git worktrees. |
| `/soul` · `/prompt` | Change response style or the system prompt persona. |
| `/language` · `/image` | Switch UI language or attach a local image. |
| `/help` | Show the complete command and keybinding reference. |

Useful keys:

- `Shift+Tab`: cycle through `default → accept_edits → yolo → auto → dont_ask`. The Chinese UI labels are “默认模式 / 接受编辑 / YOLO / 自动模式 / 不询问”, and each manual switch shows a yellow explanation above the status line for about six seconds. `dont_ask` rejects actions that would otherwise prompt; it is not YOLO.
- `Ctrl+O`: toggle compact and detailed tool output.
- `Ctrl+E`: focus the full content of the selected tool item.
- `Shift+Enter`: insert a newline in the composer.
- Paste content: keep up to 1000 characters visible; collapse larger pastes to `[Pasted Content N chars]` and expand them on submit.
- `Esc`: interrupt the current turn or leave a focused view.

While the model is working, type the next message and press Enter. It remains visible above the composer and is sent when the current turn finishes.

## Extending LubanCode

There are four extension paths:

1. **Skills**: project or user-level `SKILL.md` packages.
2. **MCP / LSP**: stdio tool servers and language servers configured in JSON.
3. **Lua plugins**: one Lua file becomes one model-callable tool.
4. **C ABI plugins**: in-process native libraries (`.dll` / `.so` / `.dylib`) for native integrations.

See the [extension guide](docs/features/extensions/README.md) for layouts, examples, namespacing, and security boundaries.

## Documentation

| Document | Contents |
| --- | --- |
| [Documentation index](docs/README.md) | Reading paths, status, and links to each guide. |
| [Three-minute quickstart](docs/getting-started/quickstart.md) | Install, connect a model, run a task, and inspect the result. Chinese. |
| [Why LubanCode](docs/getting-started/why-lubancode.md) | C++ trade-offs and a detailed comparison with Claude Code, Codex CLI, and Pi. Chinese. |
| [Configuration](docs/reference/configuration.md) | Providers, precedence, hooks, MCP, web search, LSP and model catalogs. Chinese. |
| [Extensions](docs/features/extensions/README.md) | Skills, Lua, C ABI plugins, MCP and LSP. Chinese. |
| [Architecture](docs/architecture/README.md) | Layers, request flow, API backends, tools and platform boundaries. Chinese. |
| [Terminal UX](docs/features/terminal/README.md) | Work animation, queued input, `ask_user`, approvals and edit matching. Chinese. |
| [Project instructions](docs/features/project-instructions/README.md) | `/init`, `AGENTS.md` layering, overrides and size limits. Chinese. |
| [Prompt modules](src/prompts/README.md) | How built-in prompts are split, embedded, seeded and overridden. Chinese. |

## CI and releases

Code changes pushed to the repository or submitted through a pull request are built and tested on:

- Windows with MSVC
- Ubuntu with GCC
- macOS with Clang

Pushing a `v*` tag builds all three release archives, creates a GitHub Release, generates release notes, and uploads the binaries:

```bash
version=vX.Y.Z
git tag -a "$version" -m "$version"
git push origin "$version"
```

## License

LubanCode is licensed under the [Apache License 2.0](LICENSE). You may use, modify, distribute, and commercially deploy the code subject to its attribution, change-notice, and patent terms.
