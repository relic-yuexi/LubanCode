# terminal

## description

Interactive terminal sessions on a real PTY (Linux/macOS only). Use it to drive programs that need a keyboard and a screen: vim/nano, less, python/sqlite/node REPLs, interactive installers and prompts, ssh, gdb, top. For ordinary one-shot commands keep using run_command — it is faster and returns full output.

Actions: open starts a bash session (sh if bash is missing) and returns its session_id plus the first screen; send types keys and returns the screen once output settles; read returns the current screen (optionally waiting for new output); close ends the session (SIGHUP, then SIGTERM, then SIGKILL); list shows open sessions. At most 8 sessions at a time; close the ones you no longer need.

Key notation inside keys: <Enter> <Tab> <S-Tab> <Esc> <BS> <Space> <Up> <Down> <Left> <Right> <Home> <End> <PageUp> <PageDown> <Insert> <Delete> <F1>..<F12> <C-a>..<C-z> (e.g. <C-c> interrupt, <C-d> EOF, <C-z> suspend) <M-x> (Alt). Names are case-insensitive. Write a literal "<" as <lt>, or pass literal:true to send keys verbatim. An unknown <Name> is rejected rather than typed.

The result is a plain-text snapshot of the visible screen (colors dropped) with a header showing pid, running/exited (with exit code), size, cursor position and whether a full-screen program is on the alternate screen. Pass history_lines to also get lines that scrolled off the top.

Typical flow: open → send "vim notes.txt<Enter>" → send "ihello<Esc>:wq<Enter>" → close. For a REPL: send "python3<Enter>", then send "1+1<Enter>" and read the answer from the screen. If a command is still running when send returns, call read with wait_ms instead of re-sending.

## param.action

open / send / read / close / list.

## param.session_id

Session number returned by open. Required for send, read and close.

## param.keys

Keys to type for send. Plain text plus <Name> key notation (see description); <Enter> submits a line.

## param.literal

If true, send keys exactly as given without interpreting <...> notation.

## param.wait_ms

Maximum wait in milliseconds before returning the screen; returns early once new output has been quiet for 300 ms. Default 1000 for open/send, 0 for read; capped at 60000.

## param.cwd

Working directory for open. Defaults to the current working directory.

## param.rows

Screen rows for open. Default 40 (5..100).

## param.cols

Screen columns for open. Default 160 (20..400).

## param.history_lines

Also include up to this many lines that scrolled off the top of the main screen. Default 0, max 2000.
