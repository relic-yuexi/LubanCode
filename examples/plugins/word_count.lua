-- word_count - lubancode Lua 插件示例(M7)。
--
-- 放进 <主目录>/.lubancode/plugins/ 即挂载,工具名是
-- plugin__word_count__word_count。数词数:连续非空白算一个词
-- (中文没有空格分词,整段连写算一个词,这只是个示例)。
return {
    name = "word_count",
    description = "统计文本里的词数(连续非空白算一个词)。入参 {\"text\": string}。",
    input_schema = [[{
        "type": "object",
        "properties": {
            "text": { "type": "string", "description": "要统计的文本" }
        },
        "required": ["text"]
    }]],
    execute = function(input)
        local text = input.text
        if type(text) ~= "string" then
            error("入参里缺 text 字段(要 {\"text\": \"...\"})")
        end
        local count = 0
        for _ in string.gmatch(text, "%S+") do
            count = count + 1
        end
        return "词数: " .. count
    end,
}
