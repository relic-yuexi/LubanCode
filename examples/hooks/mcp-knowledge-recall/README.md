# mcp-knowledge-recall:MCP 组合样例(PostUser)

"每次提问前查我的知识库"的完整答案。演示:

- Lua 调外部能力不另起协议:`luban.tools.list`/`call` 走宿主统一执行
  服务到达已注册的 MCP 工具(注册表解析、schema 复验、准入、递归治理
  全在宿主侧);
- 每次子执行独立 `executionId` 留账(tool.execution.* 全链,linkage 带
  hookInvocationId)——不冒充模型 tool_call,不向 provider 塞孤立
  tool message;需要进上下文时经 `luban.context.append` 提交带来源的
  正式候选;
- 结果未知的分支写全:`status == "unknown"`(超时/断连)不自动重试,
  标注不确定让人核对;准入被拒安静放行(keep_original)。

生产接入:把包装到 `~/.lubancode/hooks/`,并在宿主把 `kb.search` 加进
本包的授权名单——发现不等于授权。fixtures 里是 fake 工具;真实 MCP 服务
的联调须在会话内沿既有授权观察,报告会明列未验。跑法:
`lubancode hook test .`。
