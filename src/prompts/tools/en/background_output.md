## description

Check the status and output of background commands (the ones started with run_command run_in_background:true). Without task_id it lists a summary of every background task: task_id, status (running/completed/failed/stopped), command, PID and log file path. With task_id it returns that task's details plus the last tail_lines lines of the log file (50 by default). Reading works while a task is still running; the file may be read as it is being written. After starting a background command, use this tool to check progress or results instead of assembling your own tail command.

## param.task_id

The background task id to look up (the numeric string returned by run_command in background mode). Omit it to list a summary of all background tasks.

## param.tail_lines

How many lines to read from the end of the log file; 50 by default. Only used when task_id is given; <=0 means read the whole file (capped at 64KB).
