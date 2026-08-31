## description

Search a directory or a single file, in two modes: mode="grep" searches file contents by regex (Rust regex syntax, the same engine ripgrep uses) and returns matching lines as file:line:content; mode="glob" finds files by name wildcard (supports * ? **) and returns a list of relative paths. Searching starts from the current working directory by default, respects .gitignore/.ignore/.rgignore, automatically skips .git/, build/, node_modules/, .evidence/ (runtime observation records) and binary files, while hidden project files (e.g. .github/, .clang-format) remain searchable. To search ignored files or observation records, name the exact file or directory in path. Results are truncated at 100 hits by default, with a note when that happens (declare max_results to ask for fewer); a single over-long matching line is truncated at 16 KiB. Always use this tool for searching; NEVER invoke rg or grep via run_command.

## param.mode

"grep" searches file contents (regex); "glob" finds files by name (wildcards)

## param.pattern

With mode=grep this is a Rust regex regular expression (the ripgrep engine: no lookahead or backreferences; Unicode property escapes like \p{Han} are supported — pass fixed_strings=true to match metacharacters literally); with mode=glob it is a filename wildcard (supports * ? **, ripgrep globset syntax). A pattern without '/' (e.g. *.md) matches against the file name and recursively finds every file with that name anywhere in the tree, no matter how deep; a pattern with '/' (e.g. src/**/*.hpp, docs/**) matches against the relative path, where '**/' means zero or more directories — written at the front it means "the root directory counts too".

## param.path

Where to search: a directory (searched recursively) or a single file (only that file is searched); omit for the current working directory

## param.glob

Only effective with mode=grep: filter the files to search by name or path; omit to search every non-binary file. Same semantics as the glob form of pattern: entries without '/' like *.cpp match recursively by file name in any directory; entries with '/' like src/**/*.hpp match by relative path.

## param.fixed_strings

Only effective with mode=grep: when true, pattern is matched literally with no regex metacharacters; defaults to false, meaning pattern is parsed as Rust regex

## param.max_results

Declare up front how many results you want (a soft request that can only lower the cap): the search stops once reached and notes the truncation; defaults to the hard cap of 100. grep counts matched lines, glob counts files
