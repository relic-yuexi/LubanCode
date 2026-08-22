## description

Stop a background command (one started with run_command run_in_background:true). On Windows it terminates the root process with TerminateProcess; on POSIX it kills the whole process group. A task that has already finished is not killed again. Use it when a long-lived process (dev server, watch, build) has run long enough, or when one was started by mistake and needs winding up.

## param.task_id

The background task id to stop (the numeric string returned by run_command in background mode).
