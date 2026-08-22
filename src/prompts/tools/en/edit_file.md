## description

Replace strings in an existing file: an exact match is tried first; if that fails, limited fallbacks apply (CRLF/LF, unified indentation, trailing whitespace). old_string must still match uniquely (unless replace_all is set to true); when several candidates exist, the tool never guesses. Suited to small, precise changes; not for rewriting a whole file (use write_file for that). Requires user confirmation before executing.

## param.path

File path to modify; relative or absolute both work

## param.old_string

The original text to be replaced. A verbatim match is preferred; when necessary, line endings, indentation and trailing whitespace are tolerated — but the match must still be unique

## param.new_string

The new content to substitute in

## param.replace_all

true replaces every occurrence; omit for the default false (a unique match is required)
