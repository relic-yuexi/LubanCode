## description

Write content to a file (UTF-8 encoded). If the file already exists it is overwritten entirely; missing parent directories are created automatically. The path may be relative or absolute. Best for creating new files or rewriting a whole file; for small targeted changes, edit_file is more precise. Requires user confirmation before executing.

## param.path

File path to write; relative or absolute both work

## param.content

File content to write (UTF-8); replaces the original file entirely
