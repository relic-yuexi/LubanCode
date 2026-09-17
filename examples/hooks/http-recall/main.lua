-- http-recall:PostUser 受控 HTTP 召回(挂载个人知识库一角)。
-- user 接纳后独立触发:取回材料,经 luban.context.append 提交隐藏上下文
-- 候选——不能回写原 user(§4.47),采用与否由宿主按效果管道裁决。
--
-- Host API 的失败形状是 nil + err 表(code/message):门没开是
-- capability_not_granted,网络是 network_failed。失败不炸场:keep_original
-- 放行原文,记一条结构化日志。
local KB_URL = "https://kb.local/notes"

local function recall_notes()
  local resp, err = luban.http.request({method = "GET", url = KB_URL, timeout_ms = 1500})
  if resp == nil then
    return nil, err
  end
  if resp.status ~= 200 then
    return nil, {code = "http_" .. resp.status, message = "知识库回码 " .. resp.status}
  end
  return resp.body, nil
end

return {
  recall = function(ctx, input, next)
    local notes, err = recall_notes()
    if notes == nil then
      -- 召回失败不拦会话:原文照走,日志留痕(显示投影归宿主)。
      luban.log.event("warn", {hook = ctx.hookId, what = "recall_failed",
                               code = tostring(err and err.code or "?")})
      return next(input)
    end
    -- 同一条材料不重复追加:state 由宿主持有,跨 invocation 可见。
    local seen = false
    local ok_get, prev = pcall(luban.state.get, "last_notes_digest")
    if ok_get and type(prev) == "string" then
      seen = prev == notes
    end
    if not seen then
      luban.context.append("知识库补充材料:\n" .. notes, "knowledge.recall_http")
      pcall(luban.state.set, "last_notes_digest", notes)
    end
    return next(input)
  end,
}
