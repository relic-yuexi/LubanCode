## description

Run semantic queries through an LSP language server: mode=definition finds the definition (requires line/character), mode=references finds references (requires line/character), mode=symbols lists the symbols in a file, mode=diagnostics shows a file's diagnostics (errors/warnings). line/character are 1-based, matching what the editor displays. Only languages configured in the lsp section of config (routed by file extension) can be queried.

## param.mode

Query type

## param.file

File path to query (relative or absolute)

## param.line

Line number, 1-based (required for definition/references)

## param.character

Column number, 1-based (required for definition/references)
