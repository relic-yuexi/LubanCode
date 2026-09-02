## description

Run one command in a shell and get the merged stdout/stderr plus the exit code. The shell parameter accepts powershell (default) or cmd; write the command in the syntax of whichever you pick. User confirmation is required before executing. Commands that time out are force-killed. Build, configure and test commands default to 900000 ms (15 minutes), while other commands default to 120000 ms (2 minutes); for commands expected to run longer, pass timeout_ms explicitly or use run_in_background=true and poll with background_output. For long-lived processes that must survive across commands and calls — a dev server, a watch process — or for a short task you want to finish in the background without blocking the conversation, pass run_in_background=true: the tool does not wait, and returns a task_id, a PID and a log file path as soon as the spawn succeeds; when the command finishes, a one-line completion notice is printed at the next prompt. Afterwards use the background_output tool (pass task_id) to check status and read output, and the stop_background tool to wind it up.

## description (POSIX)

Run one command in a shell (/bin/sh) and get the merged stdout/stderr plus the exit code. Write the command in POSIX sh syntax. User confirmation is required before executing. Commands that time out are force-killed. Build, configure and test commands default to 900000 ms (15 minutes), while other commands default to 120000 ms (2 minutes); for commands expected to run longer, pass timeout_ms explicitly or use run_in_background=true and poll with background_output. For long-lived processes that must survive across commands and calls — a dev server, a watch process — or for a short task you want to finish in the background without blocking the conversation, pass run_in_background=true: the tool does not wait, and returns a task_id, a PID and a log file path as soon as the spawn succeeds; when the command finishes, a one-line completion notice is printed at the next prompt. Afterwards use the background_output tool (pass task_id) to check status and read output, and the stop_background tool to wind it up.

## param.command

The command to execute; write it in the syntax of the chosen shell (PowerShell syntax by default)

## param.command (POSIX)

The command to execute; write it in POSIX sh syntax

## param.timeout_ms

Timeout in milliseconds. If omitted, build/configure/test commands default to 900000 (15 minutes), while other commands default to 120000 (2 minutes); explicitly raise this value or use run_in_background for long commands

## param.shell

Which shell to execute with; omit for the default powershell

## param.shell (POSIX)

Which shell to execute with; this platform only has sh (/bin/sh) — powershell/cmd are Windows-only

## param.run_in_background

true = run in the background; return without waiting for the command to finish. Use it for long-lived processes that must survive across commands and multiple turns of calls — a dev server, a watch process — because after starting one you will go on verifying it with other commands (curl, say); also use it for a short task you want to run without blocking the current conversation, notified once it finishes. The result returns immediately after a successful spawn and contains a task_id, the child process PID and a log file path (the process's merged stdout/stderr is written to that file); when the command finishes, a one-line completion notice is printed at the next prompt. To see whether it is still alive and what it has printed, use the background_output tool (pass task_id) to check status and read output; to wind it up, use the stop_background tool. The timeout_ms parameter is meaningless in this mode and is ignored. Omit for the default false (foreground execution: wait for the command to finish and take the full output and exit code).

## param.cwd

Working directory for the command; relative or absolute both work. If omitted, the current session working directory is used. The directory must actually exist. In a session living inside an isolated worktree, directories pointing at the main checkout are rejected
