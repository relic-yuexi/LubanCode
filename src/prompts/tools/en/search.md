## description

Search a directory or a single file, in two modes: mode="grep" searches file contents by regex (ECMAScript syntax) and returns matching lines as file:line:content; mode="glob" finds files by name wildcard (supports * ? **) and returns a list of relative paths. Searching starts from the current working directory by default and automatically skips .git/, build/, node_modules/, .evidence/ (runtime observation records) and binary files. Results are truncated at 100 hits, with a note when that happens. To search observation records, name the exact file or directory in path.

## param.mode

"grep" searches file contents (regex); "glob" finds files by name (wildcards)

## param.pattern

With mode=grep this is an ECMAScript regular expression; with mode=glob it is a filename wildcard (supports * ? **). A pattern without '/' (e.g. *.md) matches against the file name and recursively finds every file with that name anywhere in the tree, no matter how deep; a pattern with '/' (e.g. src/**/*.hpp, docs/**) matches against the relative path, where '**/' means zero or more directories — written at the front it means "the root directory counts too".

## param.path

Where to search: a directory (searched recursively) or a single file (only that file is searched); omit for the current working directory

## param.glob

Only effective with mode=grep: filter the files to search by name or path; omit to search every non-binary file. Same semantics as the glob form of pattern: entries without '/' like *.cpp match recursively by file name in any directory; entries with '/' like src/**/*.hpp match by relative path.
