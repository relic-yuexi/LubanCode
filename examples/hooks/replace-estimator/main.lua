-- replace-estimator:同名替换内置估算槽位(PreRequest/estimate)。
-- 逻辑键 (hookPoint, name) 与内置 context.token_estimate 同键:用户层
-- (user)压过内置(builtin),获选项进计划、内置退出执行计划——旁路
-- 请求(compact/title 的估算)走同一槽,不带第二份公式。
--
-- 槽位合同不许解除:估算只产出结构化测量结果(EST1 形状);estimate
-- 阶段输入已冻结,改写请求一律拒。这里给一版 CJK 友好的字符口径:
-- 全文按 UTF-8 码点计数,CJK 当 1 字符(≈1.5 token 的粗账,系数可调)。
local CJK_TOKENS_PER_CHAR = 1.5
local OTHER_TOKENS_PER_CHAR = 0.25

local function text_of(value)
  if type(value) == "string" then
    return value
  end
  if type(value) ~= "table" then
    return ""
  end
  local parts = {}
  for _, item in ipairs(value) do
    if type(item) == "table" and type(item.text) == "string" then
      parts[#parts + 1] = item.text
    elseif type(item) == "string" then
      parts[#parts + 1] = item
    end
  end
  return table.concat(parts, "\n")
end

local function count_cjk_runes(s)
  local count = 0
  for _, code in utf8.codes(s) do
    if code >= 0x2E80 then  -- CJK 区块及以远的全宽文字(粗口径)
      count = count + 1
    end
  end
  return count
end

return {
  estimate = function(ctx, input, next)
    local pieces = {}
    if type(input.system) == "string" then
      pieces[#pieces + 1] = input.system
    end
    if type(input.messages) == "table" then
      for _, message in ipairs(input.messages) do
        pieces[#pieces + 1] = text_of(message.content)
      end
    end
    local all = table.concat(pieces, "\n")
    local cjk = count_cjk_runes(all)
    local rune_count = 0
    for _ in utf8.codes(all) do
      rune_count = rune_count + 1
    end
    local other = rune_count - cjk
    local tokens = math.ceil(cjk * CJK_TOKENS_PER_CHAR + other * OTHER_TOKENS_PER_CHAR + 0.5)
    -- 返回形状:表只认 output/effects 两键——测量结果裹进 output 才是
    -- handler 的产出值(裸字段会被当形状忽略)。
    return {
      output = {
        estimator = "cjk_runes_v1",
        estimatorVersion = 1,
        scope = "model_input_json_utf8_v1",
        encoding = "utf-8",
        rounding = "ceil",
        inputUtf8Bytes = #all,
        inputRuneCount = rune_count,
        estimatedInputTokens = tokens,
        coverage = "partial",
      },
    }
  end,
}
