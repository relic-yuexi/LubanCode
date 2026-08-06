<p align="center">
  <img src="docs/assets/lubancode-banner.png" alt="LubanCode" width="100%">
</p>

<h1 align="center">LubanCode</h1>

<p align="center"><strong>A native AI coding CLI built with C++23. Three API protocols, real tools, and a terminal interface made for sustained work.</strong></p>

<p align="center">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

<p align="center">
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/release.yml"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/release.yml/badge.svg" alt="Release"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23">
  <img src="https://img.shields.io/badge/version-0.24.1-CB2C31" alt="v0.24.1">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-444444" alt="Windows, Linux and macOS">
</p>

LubanCode connects natively to Anthropic Messages, OpenAI Responses, and OpenAI-compatible Chat Completions. It can inspect a repository, edit files, run commands, search the web, delegate work, and use MCP or LSP tools. Its terminal UI includes streaming Markdown, diff previews, approval modes, session history, and context compaction.

The name comes from Lu Ban, the traditional Chinese master craftsman. The idea is simple: measure first, cut second, and always show the work.

> Current release line: `v0.24.1`. The full suite builds and runs on Windows, Ubuntu, and macOS.

## At a glance

| Area | What is included |
| --- | --- |
| **Model access** | Anthropic, Responses, and Chat Completions; a bundled provider catalog; remembered endpoint switching. |
| **Coding tools** | Read, write, tolerant block editing, file search, foreground and background commands, diff-first approval. |
| **Semantic tools** | LSP definitions, references, symbols and diagnostics; MCP stdio; web search and fetch. |
| **Agent workflow** | Sub-agents, todo tracking, `ask_user` questions, `AGENTS.md` project instructions, deferred tools, worktrees and project permissions. |
| **Terminal UX** | Incremental Markdown rendering, animated work status, a persistent queue, collapsed multiline paste, focused views and compact output. |
| **Context and sessions** | Token breakdowns, automatic compaction, a dedicated compaction model, resume, titles, Markdown export, and opt-in project memory. |
| **Extensibility** | Skills, Lua tools, C ABI DLL plugins, hooks, themes, i18n, souls and system prompt overrides. |

## Installation

### Windows: one command

Run this from PowerShell 5.1 or newer. It downloads the latest Windows release, installs it for the current user, and updates the user PATH:

```powershell
irm https://raw.githubusercontent.com/relic-yuexi/LubanCode/main/scripts/install.ps1 | iex
```

The default destination is `%LOCALAPPDATA%\Programs\lubancode`. No administrator shell is required. Running the installer again performs an in-place upgrade.

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

The script uses `~/.local/bin` when it is already on PATH. Otherwise it falls back to `/usr/local/bin` and tells you when `sudo` is needed.

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

Run `lubancode` without arguments. When no provider has been configured, the setup wizard asks for a language, wire protocol, base URL, API key, and model, then opens the interactive session immediately.

```text
$ lubancode
=== lubancode first-run setup ===
Language: 1) 中文  2) English
Wire protocol: 1) anthropic  2) responses  3) chat_completions
base_url: https://your-provider.example/v1
api_key: sk-...
model: your-model
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

For multiple endpoints, run `/provider add`. Pick OpenAI, Anthropic, MiniMax, GLM, Qwen, DeepSeek, Kimi, or Grok, then enter the key; the repository catalog supplies the URL, wire, default model, limits, and provider options. The final menu item keeps the fully custom wizard. Hand-written configuration remains supported:

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

Save it as `~/.lubancode/config.json`, then set `WORK_MODEL_API_KEY`. A successful `/provider switch work` also writes `active_provider`, so the next launch uses the same endpoint. The detailed [configuration guide](docs/configuration.md) is currently written in Chinese, but all field names and examples are language-neutral.

The catalog source lives in [`catalog/providers.json`](catalog/providers.json), with its schema in [`catalog/providers.schema.json`](catalog/providers.schema.json). The executable embeds a snapshot and caches validated updates under `~/.lubancode/cache/`.

## Project instructions

Run `/init` inside a repository. LubanCode creates `AGENTS.md` at the Git root with a practical scaffold for layout, build, tests, and working rules. Existing instructions are never overwritten. The current main agent and sub-agents reload the file immediately.

At startup, LubanCode walks from the Git root to the working directory. Each directory prefers `AGENTS.override.md`, then `AGENTS.md`; nearer files appear later and take precedence. Empty files are skipped, and combined instruction content is capped at 32 KiB. See [Project instructions](docs/project-instructions.md).

## Everyday commands

| Command | Purpose |
| --- | --- |
| `/provider` | Add from the catalog, refresh it, list, switch, edit, or remove endpoints. |
| `/init` | Create and load project-level `AGENTS.md`. |
| `/model` · `/think` | Change the active model and reasoning effort. |
| `/context` · `/compact` | Inspect context use and compact conversation history. |
| `/skills` · `/skill` | Manage skills under `~/.lubancode/skills`; run `/skill` bare for install examples. |
| `/mcp` · `/lsp` · `/plugins` | Inspect external tools and language servers. |
| `/tools` · `/todos` | Inspect tool loading state and the current task list. |
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

See the [extension guide](docs/extensions.md) for layouts, examples, namespacing, and security boundaries.

## Documentation

| Document | Contents |
| --- | --- |
| [Documentation index](docs/README.md) | Reading paths, status, and links to each guide. |
| [Configuration](docs/configuration.md) | Providers, precedence, hooks, MCP, web search, LSP and model catalogs. Chinese. |
| [Extensions](docs/extensions.md) | Skills, Lua, C ABI plugins, MCP and LSP. Chinese. |
| [Architecture](docs/architecture.md) | Layers, request flow, API backends, tools and platform boundaries. Chinese. |
| [Terminal UX](docs/terminal-ui.md) | Work animation, queued input, `ask_user`, approvals and edit matching. Chinese. |
| [Project instructions](docs/project-instructions.md) | `/init`, `AGENTS.md` layering, overrides and size limits. Chinese. |
| [Prompt modules](src/prompts/README.md) | How built-in prompts are split, embedded, seeded and overridden. Chinese. |

## CI and releases

Every push and pull request is built and tested on:

- Windows with MSVC
- Ubuntu with GCC
- macOS with Clang

Pushing a `v*` tag builds all three release archives, creates a GitHub Release, generates release notes, and uploads the binaries:

```bash
git tag -a v0.24.1 -m "v0.24.1"
git push origin v0.24.1
```

## License

No open-source license has been added yet. Until a `LICENSE` file is published, all rights remain reserved by the author.
