// 拉取"某个 wire + base_url + api_key 下有哪些模型可用"。这是一个自由函数,
// 不进 Backend 抽象接口——Backend 只管"发消息、收流式事件"这一件事,列模型
// 是配置阶段(初次配置向导、/model 命令)用得上的另一件事,没必要塞进
// send_stream 那套接口里搅在一起。
//
// 两种 wire 的端点、响应体格式都不一样:
//   - anthropic wire:GET {base_url}/v1/models,响应 {"data":[{"id","display_name",...}]}
//   - responses wire:GET {base_url}/models,响应 {"object":"list","data":[{"id",...}]}
// JSON 解析拆成两个纯函数(ParseAnthropicModelsResponse / ParseResponsesModelsResponse),
// 不碰网络,好单测;ListModels 才是真正发 HTTP GET 的地方。

#pragma once

#include <expected>
#include <string>
#include <vector>

#include "api/types.hpp"
#include "config/config.hpp"

namespace lubancode::api {

// 一个可用模型。display_name 缺省时(responses wire 从不给这个字段;anthropic
// wire 偶尔也可能没有)用 id 兜底,调用方不用自己判断"到底该显示哪个"。
struct ModelInfo {
    std::string id;
    std::string display_name;
};

// 纯函数:解析 anthropic wire 的 GET /v1/models 响应体
// ({"data":[{"id","display_name",...}]})。data 不是数组、某个元素没有 id、
// 或者整体不是合法 JSON,都返回带说明的错误。
std::expected<std::vector<ModelInfo>, std::string> ParseAnthropicModelsResponse(const std::string& json_text);

// 纯函数:解析 responses wire 的 GET /models 响应体
// ({"object":"list","data":[{"id",...}]})。没有 display_name 字段,直接拿
// id 当 display_name。
std::expected<std::vector<ModelInfo>, std::string> ParseResponsesModelsResponse(const std::string& json_text);

// 真正发请求:按 wire 挑端点和解析函数,GET 请求带
// `Authorization: Bearer {api_key}`。网络错、HTTP 非 2xx、响应体解析失败,
// 统一走 Error 返回(跟 Backend::send_stream 用同一套 Error 类型)。
// M11(网络超时):这是个非流式请求(响应体就是一个模型列表,不会很大),
// 没有"回复很长"的顾虑,直接给连接超时 + 整体超时两道上限,都有默认值
// (来自 config::kDefault*),两个调用点(初次配置向导、/model)目前都用
// 默认值——向导阶段还没有 Config 对象,/model 命令懒得为这一个次要路径
// 多传一个字段,默认值本身已经够用。
std::expected<std::vector<ModelInfo>, Error> ListModels(config::Wire wire, const std::string& base_url,
                                                          const std::string& api_key,
                                                          int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                                                          int request_timeout_secs = config::kDefaultRequestTimeoutSecs);

}  // namespace lubancode::api
