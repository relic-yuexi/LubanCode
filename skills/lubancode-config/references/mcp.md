# MCP 服务器

### mcpServers

`mcpServers` 的键是服务器名，值是服务器设置。`command` 必填，为字符串；`args` 可选，为字符串数组；`env` 可选，为字符串到字符串的 object。

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node",
      "args": ["C:/tools/mcp-server.js"],
      "env": { "MCP_MODE": "stdio" }
    }
  }
}
```

### 挂 MCP 服务器

编辑 `~/.lubancode/config.json`，在顶层添或合并 `mcpServers`。服务器名自定，改 `command`、`args`、`env`：

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node",
      "args": ["C:/tools/mcp-server.js"],
      "env": { "MCP_MODE": "stdio" }
    }
  }
}
```

`command` 必填；不需参数或环境变量，就删去 `args` 或 `env`。
