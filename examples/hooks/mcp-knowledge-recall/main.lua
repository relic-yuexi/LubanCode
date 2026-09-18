-- mcp-knowledge-recall:MCP 组合样例(PostUser)。
-- Lua 需要外部能力时不另起协议:经宿主工具调用桥(luban.tools)使用已
-- 注册的 MCP 工具。路径是 Lua -> 宿主统一执行服务 -> 注册表解析/准入 ->
-- MCP client -> 服务端;每次子执行独立 executionId 留账,不冒充模型的
-- tool_call,也不向 provider 塞孤立 tool message。
--
-- 三条分支都要写全(§5.1):
--   finished   正常取材料,提交隐藏上下文候选;
--   unknown    超时/断连——远端可能已执行,不自动重试,标注不确定;
--   rejected   准入被拒(名单外/超帽)——按本地配置决定放行还是 deny。
local KB_TOOL = "kb.search"

return {
  recall = function(ctx, input, next)
    local listed = luban.tools.list()
    local known = false
    for _, tool in ipairs(listed or {}) do
      if tool.name == KB_TOOL then
        known = true
        break
      end
    end
    if not known then
      -- 工具没授权给本 hook:不是错误,安静放行(原文照走)。
      luban.log.event("info", {hook = ctx.hookId, what = "kb_tool_not_authorized"})
      return next(input)
    end

    local query = type(input.prompt) == "string" and input.prompt or ""
    local result, err = luban.tools.call(KB_TOOL, {query = query, limit = 3})
    if result == nil then
      -- 准入被拒:err.code 是稳定码(hook.tool.not_allowed / call_cap …)。
      luban.log.event("warn", {hook = ctx.hookId, what = "kb_call_rejected",
                               code = tostring(err and err.code or "?")})
      return next(input)
    end

    if result.status == "finished" and not result.isError then
      if result.contentTruncated then
        luban.log.event("info", {hook = ctx.hookId, what = "kb_content_truncated",
                                 executionId = result.executionId})
      end
      luban.context.append("知识库检索结果(executionId=" .. result.executionId .. "):\n" .. result.content,
                           "knowledge.recall")
      return next(input)
    end

    if result.status == "unknown" then
      -- 结果未知:远端可能已执行。不重试、不照实猜;给一条不确定标注,
      -- 让人(或后续轮)决定要不要人工核对。
      luban.log.event("error", {hook = ctx.hookId, what = "kb_result_unknown",
                                executionId = result.executionId})
      luban.context.append("[知识库检索结果未知(executionId=" .. result.executionId ..
                           "),不自动重试;如需材料请人工核对]", "knowledge.recall.unknown")
      return next(input)
    end

    -- cancelled / failed / 工具自身报错:照实留痕,原文放行。
    luban.log.event("warn", {hook = ctx.hookId, what = "kb_call_" .. tostring(result.status),
                             executionId = result.executionId})
    return next(input)
  end,
}
