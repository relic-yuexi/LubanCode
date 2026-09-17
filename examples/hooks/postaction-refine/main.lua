-- postaction-refine:PostAction 结果加工。
-- 副作用已经发生:这个挂点没有准入、没有改写;能做的是对已保存的原始
-- 结果给三枚加工效果——result.supplement(补材料)/ result.replace(
-- 替换展示)/ result.filter(滤条目)。原始结果照旧在账上,派生内容另标
-- 来源,不冒充工具原话。
--
-- input 形状(挂点合同):{toolName, content, isError}。isError 的结果
-- 不加工——失败原样留给上层判断,别把错误洗成成功。
return {
  refine = function(ctx, input, next)
    -- PostAction 不改输入:next() 不带候选(nil = 原样放行,不产生
    -- input.rewrite 候选;带了反而会在账上多一枚被拒的改写效果)。
    if type(input) ~= "table" or input.isError then
      return next()
    end
    if input.toolName ~= "search" then
      return next()  -- 别的工具不碰
    end
    local effects = {}
    -- 补一段使用说明(supplement:材料附加,不动原文)。
    effects[#effects + 1] = {
      type = "result.supplement",
      note = "以上为检索原文;引用时请核对来源。",
    }
    -- 检索结果太长时给一份替换用的精简版(replace:展示层替换)。
    if type(input.content) == "string" and #input.content > 4000 then
      effects[#effects + 1] = {
        type = "result.replace",
        reason = "over_preview_budget",
        content = input.content:sub(1, 4000) .. "\n…(已截断,全文见原始结果)",
      }
    end
    -- next 的返回是下游表 {status, value, ...}:输出值从 value 取,
    -- 效果随 handler 返回值携带(out.effects 是下游表,不是返回形状)。
    local downstream = next()
    return {output = downstream.value, effects = effects}
  end,
}
