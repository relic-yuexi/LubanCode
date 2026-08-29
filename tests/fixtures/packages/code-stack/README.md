# code-stack:阶段 5 挂载事务的三件套

统一 Package 封装单阶段 5 的验收夹具:两件 process 插件 + 一只 stdio MCP,
全部走 Python(测试机有 python 即可,不依赖 node)。

```text
code-stack/
├── package.yaml
├── plugins/
│   ├── count-words/    plugin.json + runner.py(工具 count:数词)
│   └── reverse-text/   plugin.json + runner.py(工具 reverse:倒序)
└── mcp/
    └── ledger/         mcp.yaml + server.py(工具 ping/note)
```

- 插件按 process 协议 v1:stdin 恰好一份 JSON 请求,stdout 恰好一份 JSON
  响应,日志走 stderr。挂载事务的探针会真起一遍进程走协议。
- MCP 按行 JSON-RPC(initialize / tools/list / tools/call)。环境变量
  `LUBANCODE_TEST_MCP_PID_FILE` 设了的话,启动时把自己的 PID 写进去——
  供"回滚后进程零残留"的断言用;不设则不写,行为不变。
- 三件全起得来,整包进正式账;坏一件(broken/code-failure 那只),三件
  一件不进,已起进程全停。
