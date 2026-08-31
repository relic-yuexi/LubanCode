<p align="center">
  <img src="docs/assets/lubancode-banner.png" alt="LubanCode" width="100%">
</p>

<h1 align="center">LubanCode</h1>

<p align="center"><strong>A native AI coding CLI built with C++23. Three API protocols, real tools, and a terminal interface made for sustained work.</strong></p>

<p align="center">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

<p align="center">
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml?query=branch%3Amain+event%3Apush"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml/badge.svg?branch=main&event=push" alt="main branch CI"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-3D6DCC" alt="Apache-2.0"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-444444" alt="Windows, Linux and macOS">
</p>

LubanCode connects natively to Anthropic Messages, OpenAI Responses, and OpenAI-compatible Chat Completions. It can inspect a repository, edit files, run commands, search the web, delegate work, and use MCP or LSP tools. A searchable catalog includes presets for 75 providers. Streaming Markdown, diffs, the agent dock, queued input, and session history all live in the terminal.

The name comes from Lu Ban, the traditional Chinese master craftsman. The idea is simple: measure first, cut second, and always show the work.

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

## Why LubanCode

Five things worth putting up front.

### 1. Download and run — no separate runtime

No Node.js. No Python. No `npm install`. The Windows x64 Release executable is about 8.2 MiB, shipped as an install-free native package you can unzip and run. One command installs it on Windows; on Linux / macOS, unpack and go. Beside the main executable, the release archive carries `libexec/rg` (the bundled ripgrep search engine, MIT licensed, which powers the `search` tool), the official skills, documentation, and licenses (including third-party notices) — search works offline right after install.

### 2. Keep typing while work is in flight

Type the next message while the model is still thinking. Press Enter and it joins the queue; when the current turn closes, the next one starts. The agent dock shows which sub-agent is running, how long it has worked, and how many tools it has called. The terminal acts like a work surface, not a scrolling log.

### 3. Soul / Law prompt separation

Two prompt layers, each in charge of one thing:

- **Law** (`~/.lubancode/system_prompt.md`): the behavioral skeleton — tool calls, coding conventions, workflow rules. Stable, rarely touched.
- **Soul** (`~/.lubancode/SOUL.md`): a style overlay — "answer only in classical Chinese" or "keep every reply under three sentences." Layered on top of Law, swappable on demand.

`/soul` switches instantly. Drop alternative souls in `~/.lubancode/souls/`. Change voice without touching behavior logic; change behavior without rewriting personality. Most tools bundle their system prompt into one monolith — LubanCode splits it apart.

### 4. Project memory

LubanCode remembers your project facts and preferences — build commands, code style, pitfalls encountered. `/memory` manages recall; a background pass organizes entries over time.

One detail: the main worktree and linked worktrees created by `/worktree` share one project identity (keyed by common git dir), so they **share the same memory store**. What you teach in the main branch carries over — no re-education when you switch into an isolated worktree.

### 5. Terminal Markdown & LaTeX rendering

Markdown tables, code blocks, lists, blockquotes in model output — LubanCode renders them in place in the terminal, not dumped as raw text. LaTeX formulas render for both inline `$...$` and display `$$...$$` into terminal-displayable form. Diffs are colorized. Tool output folds. Long pastes compress.

The terminal deserves more than plain text.

## At a glance

| Area | What is included |
| --- | --- |
| **Model access** | Anthropic, Responses, and Chat Completions; presets for 75 providers; remembered endpoint switching. |
| **Coding tools** | Read, write, tolerant block editing, file search, foreground and background commands, diff-first approval. |
| **Semantic tools** | LSP definitions, references, symbols and diagnostics; MCP stdio; web search and fetch. |
| **Agent workflow** | Sub-agents, role-based model routes, Plan mode, todo tracking, `ask_user`, `AGENTS.md`, worktrees, and project permissions. |
| **Terminal UX** | Incremental Markdown rendering, animated work status, a persistent queue, collapsed multiline paste, focused views and compact output. |
| **Context and sessions** | Token breakdowns, deterministic trimming, recoverable artifacts, on-demand artifact summaries, resume, titles, and Markdown export. |
| **Extensibility** | Skills, Lua tools, C ABI DLL plugins, hooks, themes, i18n, souls and system prompt overrides. |

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

For multiple endpoints, run `/provider add`. The 75 presets open in a searchable picker. Type a provider name, select it, and enter the key; the catalog supplies the URL, wire, default model, limits, and provider options. The final item keeps the fully custom wizard. Hand-written configuration remains supported:

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

- `Shift+Tab`: cycle through `confirm`, `auto`, and `yolo` approval modes.
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
4. **C ABI plugins**: in-process Windows DLLs for native integrations.

See the [extension guide](docs/features/extensions/README.md) for layouts, examples, namespacing, and security boundaries.

## Documentation

| Document | Contents |
| --- | --- |
| [Documentation index](docs/README.md) | Reading paths, status, and links to each guide. |
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
