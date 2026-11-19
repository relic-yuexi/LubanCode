// 拉取"某个 wire + base_url + api_key 下有哪些模型可用"。这是一个自由函数,
// 不进 Backend 抽象接口——Backend 只管"发消息、收流式事件"这一件事,列模型
// 是配置阶段(初次配置向导、/model 命令)用得上的另一件事,没必要塞进
// send_stream 那套接口里搅在一起。
//
// 各家 wire 的端点、响应体格式不一样:
//   - anthropic wire:GET {base_url}/v1/models,响应 {"data":[{"id","display_name",...}]}
//   - responses/chat_completions wire:GET {base_url}/models,响应 {"object":"list","data":[{"id",...}]}
//   - google-generate-content wire:GET {base_url}/v1beta/models,响应
//     {"models":[{"name":"models/gemini-...","supportedGenerationMethods":[...]}]},
//     只留支持 generateContent 的条目,name 去掉 "models/" 前缀当 id
// JSON 解析拆成三个纯函数(ParseAnthropicModelsResponse /
// ParseResponsesModelsResponse / ParseGeminiModelsResponse),
// 不碰网络,好单测;ListModels 才是真正发 HTTP GET 的地方。

#pragma once

#include <expected>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "api/types.hpp"
// FD-05:本头只用 config::Wire 与两枚非流式超时默认值,来自
// config/model_protocol.hpp 这枚零依赖小头,不再拖整份配置合同。
#include "config/model_protocol.hpp"

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

// 纯函数:解析 google-generate-content wire 的 GET /v1beta/models 响应体
// ({"models":[{"name":"models/gemini-...","displayName":...,
// "supportedGenerationMethods":["generateContent",...]}]})。name 去 "models/"
// 前缀当 id,displayName 缺省用 name 兜底;supportedGenerationMethods 不含
// "generateContent" 的条目(embedding、aqa 这类)整个跳过——列出来也调不动。
std::expected<std::vector<ModelInfo>, std::string> ParseGeminiModelsResponse(const std::string& json_text);

// 纯函数(向导重排单):算"探测模型列表要打的完整 URL"。anthropic wire 补
// /v1/models,responses/chat_completions 补 /models,google-generate-content
// 补 /v1beta/models;anthropic 且 base_url 已带 /v1 结尾、gemini 且已带
// /v1beta 结尾时不重复再补一份(否则会拼出 /v1/v1/models 这类双节地址)。
// 向导界面展示的探测地址与 ListModels 实际请求的地址同出这一份,不许两处
// 各算各的。
std::string ModelsUrl(config::Wire wire, const std::string& base_url);

// 纯函数:ListModels 的请求头。gemini wire 走 x-goog-api-key(为空彻底不
// 带),其余 wire 走 Authorization: Bearer;api_key 为空(鉴权三态 none/
// 缺 env)都彻底不带,不发空头。extra_headers 空值删头、非空覆盖。header
// 单测钉在这(gemini client 的同款规矩在 api/gemini/client.cpp 的
// GeminiBaseHeaders,OpenAI 系三件在 api::RequestBaseHeaders)。
std::map<std::string, std::string> ModelsRequestHeaders(
    config::Wire wire, const std::string& api_key,
    const std::map<std::string, std::string>& extra_headers);

// 真正发请求:按 wire 挑端点、请求头和解析函数(地址统一出自 ModelsUrl,
// 头统一出自 ModelsRequestHeaders),网络错、HTTP 非 2xx、
// 响应体解析失败,统一走 Error 返回(跟 Backend::send_stream 用同一套
// Error 类型)。
// M11(网络超时):这是个非流式请求(响应体就是一个模型列表,不会很大),
// 没有"回复很长"的顾虑,直接给连接超时 + 整体超时两道上限,都有默认值
// (来自 config::kDefault*),两个调用点(初次配置向导、/model)目前都用
// 默认值——向导阶段还没有 Config 对象,/model 命令懒得为这一个次要路径
// 多传一个字段,默认值本身已经够用。
std::expected<std::vector<ModelInfo>, Error> ListModels(config::Wire wire, const std::string& base_url,
                                                          const std::string& api_key,
                                                          int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                                                          int request_timeout_secs = config::kDefaultRequestTimeoutSecs,
                                                          const std::map<std::string, std::string>& extra_headers = {});

// ---- 自部署端点窗口探测(Terminal-Bench 自部署单)----------------------
// 模型不在 catalog、用户也没显式配窗口时,对 OpenAI 兼容端(vLLM/SGLang/
// llama.cpp/LM Studio 一类)GET {base_url}/models 一次,读出本场模型的真
// 窗口,免得按 256k 兜底算压缩阈值、撞服务端 --max-model-len 吃 400。

// 纯函数:在 /models 响应体的 data[] 里按 id 找本场模型(先精确,再 ASCII
// 忽略大小写),依次读 max_model_len(vLLM)、context_length、context_window、
// max_context_length,取第一枚正整数。非 JSON、非对象、缺 data、找不到
// 模型、字段缺失或非正数,一律给空。
std::optional<std::size_t> ParseModelContextWindowFromModelsResponse(const std::string& json_text,
                                                                     const std::string& model_id);

// 真发一次 GET:只对 responses/chat_completions 两种 OpenAI 兼容 wire 动手
// (anthropic/gemini 直接给空)。地址与请求头出自 ModelsUrl/
// ModelsRequestHeaders,跟 ListModels 同一份;连接与总时长都钳在
// timeout_ms(≤3000)内。网络错、非 2xx、解析不出,一律静默给空——探测
// 只是锦上添花,拦不住开工。代理照 cpr/libcurl 的环境变量规矩走(同
// ListModels)。
std::optional<std::size_t> ProbeModelContextWindow(config::Wire wire, const std::string& base_url,
                                                   const std::string& api_key, const std::string& model_id,
                                                   const std::map<std::string, std::string>& extra_headers = {},
                                                   int timeout_ms = 3000);

}  // namespace lubancode::api
