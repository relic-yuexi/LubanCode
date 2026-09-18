-- business-deny:PreUser 业务拒绝(准入决定)。
-- 演示 ctx.deny(code, message):返回拒绝标记表即短路——本轮输入不被
-- 接纳,后续 handler 不进(记 skipped_short_circuit),链尾零次。业务
-- deny 与脚本错误分开:deny 走 DispatchOutcome::Denied,不落错误码表,
-- 用户看得到理由。
--
-- 规则(示意):明文口令进 prompt 一律拦下,让用户改用 Secret。
local secrets_needles = {
  "sk-proj-",                 -- 常见 API key 前缀(示意,不是安全边界)
  "BEGIN RSA PRIVATE KEY",
  "BEGIN OPENSSH PRIVATE KEY",
}

local function contains_secret(text)
  for _, needle in ipairs(secrets_needles) do
    if text:find(needle, 1, true) then
      return needle
    end
  end
  return nil
end

return {
  gate = function(ctx, input, next)
    if type(input.prompt) == "string" then
      local hit = contains_secret(input.prompt)
      if hit ~= nil then
        -- 理由只说命中的是哪一类,不回显口令本体(避免把凭据再抄一遍)。
        return ctx.deny("secret_in_prompt",
                        "输入里有疑似明文凭据(命中 " .. hit .. " 一类);请改用 luban.secrets 声明的 Secret,不要把口令贴进对话。")
      end
    end
    return next(input)
  end,
}
