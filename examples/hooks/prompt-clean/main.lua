-- prompt-clean:PreUser 清洗输入候选。
-- 合同:handler(ctx, input, next)。input 是挂点专用候选副本,本地怎么改
-- 都不碰 session;改写要经 next(candidate) 提交,宿主验证后才算采用。
-- 返回 next 的下游值(透传)。
--
-- 局部变量不跨调用:每次 invocation 都是全新 Lua state,别想在这里攒
-- 计数器——要存状态用 luban.state(见 http-recall 示例)。
local function trim(s)
    return (s:gsub("^%s*(.-)%s*$", "%1"))
end

-- 把三个以上连续空行压成一行(保留段落感,不把全文压成一坨)。
local function squeeze_blank_lines(s)
    return (s:gsub("\n%s*\n%s*\n+", "\n\n"))
end

return {
  normalize = function(ctx, input, next)
    if type(input.prompt) ~= "string" then
      return next(input)  -- 不是文本输入:原样放行
    end
    local candidate = {}
    for key, value in pairs(input) do
      candidate[key] = value  -- 其他字段原样带过去
    end
    candidate.prompt = squeeze_blank_lines(trim(input.prompt))
    if candidate.prompt == input.prompt then
      return next(input)  -- 本来就干净:不造无谓的改写版本
    end
    return next(candidate)
  end,
}
