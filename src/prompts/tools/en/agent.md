## description

Delegate independent tasks to subagents. First think of a short semantic title (4~16 Chinese characters, or 2~6 English words) and put it in title—a noun phrase or short command that distinguishes tasks from each other; do not copy the first sentence of the prompt, and do not stuff in path lists or boilerplate. Then write the full task instructions into prompt. title is for humans (agent panel/logs), prompt is what the subagent executes; each has its own job. agent_type=Explore is a read-only code-search agent; general-purpose can research, run multi-step tasks, and edit code. A subagent has its own context and hands only its conclusion back to the main conversation. Execution mode is set with execution_mode (default auto): in interactive sessions, leave it at the default auto for independent tasks like exploring, generating, writing code, or researching—it runs in the background, the conclusion is delivered back automatically when done, and the main conversation can keep working; do not habitually write foreground just to get the result—background results flow back all the same; write foreground explicitly only when the very next step cannot proceed without this result. In pipe/one-shot scenarios, auto is equivalent to foreground (blocking until the conclusion). Background tasks cannot raise permission confirmations; operations not pre-approved will be rejected. The subagent cannot see the current conversation history; prompt must be self-contained.

## param.title

Short task title, required. A semantic field for humans: 4~16 Chinese characters or 2~6 English words, a noun phrase or short command that distinguishes it from other tasks. Must not copy the first sentence of the prompt; must not contain path lists, full acceptance criteria, newlines, or tabs; hard cap of 40 display columns. Write the title first, then the full prompt.

## param.prompt

The task description handed to the subagent; it must be self-contained—the subagent cannot see the main conversation history, so the goal, the scope, and the expected output form must all be spelled out.

## param.max_steps_per_turn

Maximum number of steps the subagent may run (one step = one model request; a step may contain multiple tool calls). When omitted, the configured default applies: subagent.max_steps_per_turn first, otherwise max_steps_per_turn is inherited (default 0 = no step limit). Passing 0 = no limit; a wrap-up reminder arrives when three steps remain, and at the limit the result is budget_exhausted with a checkpoint included—not a vague failure. When retrying, read the checkpoint first and narrow the scope; do not resend the same task verbatim and do not raise the step limit on your own.

## param.agent_type

Subagent type: Explore is read-only search and analysis; general-purpose can perform multi-step operations. Default general-purpose.

## param.execution_mode

Execution mode, default auto. auto: in interactive sessions the task runs independently in the background by default (the conclusion is delivered back to the main conversation automatically when done, and the main conversation can keep working)—do not habitually write foreground; write it explicitly only when the next step cannot proceed without this result; in pipe/one-shot scenarios, auto is equivalent to foreground (blocking until the conclusion). background: returns a task number immediately and runs independently in the background; background tasks cannot raise permission confirmations, and operations not pre-approved will be rejected. foreground: this call blocks until the subagent's conclusion arrives. The legacy parameter run_in_background is still accepted (true=background, false=foreground); when both are given, an explicit (non-auto) execution_mode wins.

## param.run_in_background

(Legacy parameter) Whether to run in the session background: true is equivalent to execution_mode=background, false to foreground. New calls should use execution_mode.

## param.isolation

worktree = give the subagent its own git worktree isolation room to work in: writes never touch the main checkout (file/command/git gates all block), an unchanged room is deleted automatically when done, and a changed one is kept with its path and branch attached to the result for the main agent or the user to finish up. Recommended for multi-step tasks that edit code; unnecessary for read-only surveys. Default none.

## persona.general

You are a general-purpose subagent that can search, analyze, and complete multi-step tasks. Focus on the given task and give your conclusion directly when done; no pleasantries.

## persona.explore

You are an Explore subagent that specializes in quickly searching, reading, and analyzing codebases. Read-only: you must not modify files, start commands that change the environment, or perform any other write operation. When done, give a concise conclusion with concrete file locations; no pleasantries.
