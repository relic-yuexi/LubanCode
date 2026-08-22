## description

Read file contents with a line number in front of each line (like cat -n). Use offset/limit to read only part of a file. When limit is omitted, at most 2000 lines are read by default and a single output is capped at roughly 1MB; anything beyond that is truncated and marked, and you can page onward from the truncation point with offset. The path may be relative or absolute. Only UTF-8 text is accepted (with or without a BOM; the BOM never leaks into the content). Binary files and files that are not valid UTF-8 (for example GBK-encoded ones) are rejected with a clear error—convert them to UTF-8 before reading.

## param.path

File path to read; relative or absolute both work

## param.offset

Line number to start reading from (1-based); omit to start from line 1

## param.limit

Maximum number of lines to read; omit to read through the end of the file
