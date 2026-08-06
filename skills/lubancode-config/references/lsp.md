# LSP 语言服务器

### lsp

`lsp` 的键是语言名，同时作 LSP 的 `languageId`。每项 `command` 必填且非空；`args` 可选，值为字符串数组；`extensions` 必填，为非空字符串数组；`idle_minutes` 可选，正整数，缺省 `10`。

```json
{
  "lsp": {
    "cpp": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensions": [".c", ".cc", ".cpp", ".h", ".hpp"],
      "idle_minutes": 10
    }
  }
}
```

### 开 LSP

编辑 `~/.lubancode/config.json`，在顶层 `lsp` 里按语言名加服务器。C++ 可这样写：

```json
{
  "lsp": {
    "cpp": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensions": [".cpp", ".hpp"],
      "idle_minutes": 10
    }
  }
}
```

改 `command` 为本机语言服务器命令，`extensions` 填它该接手的扩展名。`extensions` 不能空；`idle_minutes` 必须大于零。
