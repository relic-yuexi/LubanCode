## description

Delegate independent tasks to subagents. First think of a short semantic title (4~16 Chinese characters, or 2~6 English words) and put it in title—a noun phrase or short command that distinguishes tasks from each other; do not copy the first sentence of the prompt, and do not stuff in path lists or boilerplate. Then write the full task instructions into prompt. title is for humans (agent panel/logs), prompt is what the subagent executes; each has its own job. agent_type=Explore is a read-only code-search agent; general-purpose can research, run multi-step tasks, and edit code. A subagent has its own context and hands only its conclusion back to the main conversation. Execution mode is set with execution_mode (default auto): in interactive sessions, leave it at the default auto for independent tasks like exploring, generating, writing code, or researching—it runs in the background, the conclusion is delivered back automatically when done, and the main conversation can keep working; do not habitually write foreground just to get the result—background results flow back all the same; write foreground explicitly only when the very next step cannot proceed without this result. In pipe/one-shot scenarios, auto is equivalent to foreground (blocking until the conclusion). Background tasks cannot raise permission confirmations; operations not pre-approved will be rejected. The subagent cannot see the current conversation history; prompt must be self-contained.

## param.title

Short task title, required. A semantic field for humans: 4~16 Chinese characters or 2~6 English words, a noun phrase or short command that distinguishes it from other tasks. Must not copy the first sentence of the prompt; must not contain path lists, full acceptance criteria, newlines, or tabs; hard cap of 40 display columns. Write the title first, then the full prompt.

## param.prompt

(Legacy parameter) The task description handed to the subagent; it must be self-contained—the subagent cannot see the main conversation history, so the goal, the scope, and the expected output form must all be spelled out. New calls should use the task object instead: goal/context/scope/constraints/acceptance/deliverable in separate fields, which the host can validate and the panel can display in sections. Giving both task and prompt at once is rejected outright.

## param.agent_type

Subagent type: Explore is read-only search and analysis; general-purpose can perform multi-step operations (default); or a custom agent name from the /agents catalog (each with its own tool boundary, preloaded skills, and budgets—the catalog listing from /agents is authoritative).

## param.execution_mode

Execution mode, default auto. auto: in interactive sessions the task runs independently in the background by default (the conclusion is delivered back to the main conversation automatically when done, and the main conversation can keep working)—do not habitually write foreground; write it explicitly only when the next step cannot proceed without this result; in pipe/one-shot scenarios, auto is equivalent to foreground (blocking until the conclusion). background: returns a task number immediately and runs independently in the background; background tasks cannot raise permission confirmations, and operations not pre-approved will be rejected. foreground: this call blocks until the subagent's conclusion arrives. The legacy parameter run_in_background is still accepted (true=background, false=foreground); when both are given, an explicit (non-auto) execution_mode wins.

## param.run_in_background

(Legacy parameter) Whether to run in the session background: true is equivalent to execution_mode=background, false to foreground. New calls should use execution_mode.

## param.task

Structured task contract: goal and deliverable are required, everything else is optional. Mutually exclusive with prompt (giving both is rejected). Prefer it for new calls—goal, scope, constraints, acceptance, and deliverable sit in separate fields, and the subagent, the panel, and trajectories all project from this one canonical contract.

## param.task.goal

Required. What the subagent is to achieve: one thing, not a dump of the whole conversation.

## param.task.source_request

Optional. Verbatim quotes of the user's words only; the dispatcher's own guesses and summaries belong in context, never impersonating the user.

## param.task.context

Facts and background the subagent cannot see but genuinely needs: source boundaries already found, upstream decisions, error verbatim. At most 16 items.

## param.task.scope

Task scope (only for repository tasks; file-less tasks may omit it).

## param.task.scope.include_paths

Path range the task looks at (a task boundary, not a permission boundary).

## param.task.scope.exclude_paths

Paths explicitly excluded.

## param.task.constraints

Restrictions: what not to touch, no commits, read-only, etc. They never widen the sandbox. At most 16 items.

## param.task.acceptance

Acceptance criteria, one checkable condition per item. Survey tasks may write things like "give file and line numbers; separate implemented from TODO".

## param.task.deliverable

Required. What to return: a conclusion, a fix, a table, evidence, and so on. The subagent delivers against it and does not swap it for a long report.

## param.task.schema_version

Contract version, currently 1 (may be omitted).

## param.isolation

worktree = give the subagent its own git worktree isolation room to work in: writes never touch the main checkout (file/command/git gates all block), an unchanged room is deleted automatically when done, and a changed one is kept with its path and branch attached to the result for the main agent or the user to finish up. Recommended for multi-step tasks that edit code; unnecessary for read-only surveys. Default none.

## persona.general

You are a general-purpose subagent that can search, analyze, and complete multi-step tasks. Focus on the given task and give your conclusion directly when done; no pleasantries.

## persona.explore

You are an Explore subagent that specializes in quickly searching, reading, and analyzing codebases. Read-only: you must not modify files, start commands that change the environment, or perform any other write operation. When done, give a concise conclusion with concrete file locations; no pleasantries.
