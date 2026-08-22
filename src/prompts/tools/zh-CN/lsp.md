## description

用 LSP 语言服务器做语义查询:mode=definition 查定义(需要 line/character),mode=references 查引用(需要 line/character),mode=symbols 列文件里的符号,mode=diagnostics 看文件的诊断(错误/警告)。line/character 是 1 基,跟编辑器显示一致。只有 config 的 lsp 段配置过的语言(按文件扩展名路由)才能查。

## param.mode

查询类型

## param.file

要查询的文件路径(相对或绝对)

## param.line

行号,1 基(definition/references 必填)

## param.character

列号,1 基(definition/references 必填)
