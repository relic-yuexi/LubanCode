// 工具入参 JSON 的宽松解析(开源模型坏 JSON 单)。
// 开源模型走 OpenAI 兼容端点,常把工具入参写坏:包 ```json 围栏、对象/
// 数组末尾多逗号、通篇单引号、写大文件撞 max_tokens 被拦腰截断。原先
// 解析一失败就换成 {},工具照跑,模型只收到"缺参数",看不见真病因。
// 这里先严格 parse,失败再按序做有限几味修补,修成了带标记交回——修了
// 什么写进 repairs,不静默;修不成交回严格 parse 的原始报错。
//
// 修补序(逐味叠加,每味之后试 parse 一次):
//   1. 剥 markdown 代码围栏(```json ... ```);
//   2. 去字符串外、紧贴 } / ] 之前的多余逗号;
//   3. 单引号换双引号——只在整段一个双引号都没有时才试(混写的不碰);
//   4. 末尾未闭合的字符串/括号按栈补齐——补完能 parse 且结果是 object
//      才收,并置 closed_truncation(截断嫌疑:调用方遇长度截断须拒执行)。
// 额外一味:整段是一枚 JSON 字符串、里头装的才是对象(双重编码),拆一层。
// 纯函数,不依赖第三方库,不碰日志。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::api {

// 入参兜底的稳定错误码(ToolUseBlock::input_error_code / 工具终态 error_code):
//   invalid_json  —— 修补也救不回的坏 JSON;
//   truncated     —— 长度截断致入参不完整(拒执行,不补齐);
//   schema_invalid—— 执行前浅检不过(缺必填/类型明显不对)。
inline constexpr const char* kErrToolInputInvalidJson = "tool.input.invalid_json";
inline constexpr const char* kErrToolInputTruncated = "tool.input.truncated";
inline constexpr const char* kErrToolInputSchemaInvalid = "tool.input.schema_invalid";

struct ToolArgsRepairResult {
    bool ok = false;                   // 得到可用入参
    nlohmann::json value;              // ok 时的入参(严格档原样,修补档必为 object)
    bool repaired = false;             // 不是严格 parse 一次过的
    std::vector<std::string> repairs;  // 修了什么(人话,按施行序)
    bool closed_truncation = false;    // 靠补齐末尾的字符串/括号才救回
    std::string error;                 // !ok 时:严格 parse 的原始报错
};

// 空串/全空白按"无参调用"给 {}(与从前一致),不算修补。
ToolArgsRepairResult RepairToolArgs(std::string_view raw);

// 回喂模型用的原文片段:前 max_bytes 字节(UTF-8 对齐),超长补省略号。
std::string ToolArgsSnippet(std::string_view raw, std::size_t max_bytes = 200);

// 回喂模型的两份人话(工具名 + 病因 + 原文前 200 字节 + 该怎么重发)。
// 解析失败:附严格 parse 的报错原话。
std::string BuildToolArgsParseErrorText(const std::string& tool_name, const std::string& parse_error,
                                        std::string_view raw);
// 长度截断:入参半截,拒执行,点明拆小分多次写。
std::string BuildToolArgsTruncatedText(const std::string& tool_name, std::string_view raw);

// stop_reason 是否长度截断(四家 events 已归一成 max_tokens;length 防老口径)。
bool IsLengthStopReason(std::string_view stop_reason);

}  // namespace lubancode::api
