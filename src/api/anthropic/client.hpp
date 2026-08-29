// AnthropicBackend:对接 Anthropic Messages API(MiniMax 提供的兼容端点)。
// 拼 JSON、POST /v1/messages、边收边解析 SSE、逐个语义事件回调给上层。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "config/config.hpp"

namespace lubancode::api::anthropic {

// 纯函数:把中立 Request 翻译成 Anthropic Messages API 的请求体 JSON
// (stream 恒为 true)。send_stream 内部就是调这个函数拼请求体——单独在头文件
// 里声明出来,只是为了让单测能直接调用、断言拼出来的 JSON 长什么样(比如
// M6.6 的 think 强度要不要出现在 "thinking" 字段里),不碰网络、不改变任何
// 线上行为。
// native_web_search:该端(ProviderConfig::native_web_search 镜像到
// Config::native_web_search)是否声明协议原生联网搜索,默认 false,跟
// Responses 那边的同名开关是同一个配置字段,这里各协议自己翻译成各自的
// tools 数组形状。
// extra_body:Config::extra_body(顶层单 provider 配置,或者切 provider 时
// 从 ProviderConfig::extra_body 镜像过来)——浅合并进请求体顶层,merge 点
// 在所有内置逻辑(thinking/native_web_search/messages/tools)拼完之后、
// 返回之前,键冲突时 extra_body 的值整个覆盖掉前面算出来的值,不做深合并。
// 默认空 object,等于不合并任何东西,现状行为零变化。
nlohmann::json BuildRequestJson(const Request& request, bool native_web_search = false,
                                 const nlohmann::json& extra_body = nlohmann::json::object());

// 仅在“assistant(thinking + tool_use) -> user(tool_result)”这条续轮形状下
// 开原始 think 标签兼容门。纯函数，给单测钉住，免得兼容范围日后悄悄放宽。
bool ShouldRecoverTaggedThinking(const Request& request);

// 纯函数:把 extra_headers 覆盖/追加到一份基础 HTTP 头表里(key 精确匹配
// 大小写;真正发送时 cpr::Header 自身还会再做一层大小写不敏感的去重,这
// 里只管"配置里写的头名对不对得上基础头哪一条"这一步)。单独拆出来给
// 单测直接调用,不用真的发一次 HTTP 请求。
std::map<std::string, std::string> ApplyExtraHeaders(std::map<std::string, std::string> base,
                                                        const std::map<std::string, std::string>& extra_headers);

class AnthropicBackend : public Backend {
public:
    // base_url 形如 https://api.minimaxi.com/anthropic (不带结尾 /v1/messages);
    // auth_token 走 Authorization: Bearer 头(MiniMax 用这个,不是 x-api-key)。
    // M11(网络超时):connect_timeout_ms(连接超时,毫秒)、
    // stream_idle_timeout_secs(SSE 读空闲超时,秒,不是总时长上限)两个都有
    // 默认值,来自 config::kDefault*,main.cpp 用 Config 里实际生效的值调用。
    // request_hard_timeout_secs(cpr 并发挂死单):每枚请求的硬墙钟(秒,
    // 0 = 不设)。connect/idle 两道闸只各管一段,真机现场出现过两道都不响、
    // 请求永挂的病(本机代理/TUN 截胡回环),这面墙是最后一道兜底——靠
    // ProgressCallback 对 steady_clock 比期限掐流,不用 cpr::Timeout(那会把
    // 正常长流拦腰砍断)。默认值同样来自 config::kDefault*。
    // native_web_search:该端(ProviderConfig::native_web_search 镜像到
    // Config::native_web_search)是否声明协议原生联网搜索,默认 false。
    // extra_body/extra_headers:同上,从 Config 同名字段传进来,默认都是
    // 空(不合并/不加任何东西)。
    AnthropicBackend(std::string base_url, std::string auth_token,
                      int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                      int stream_idle_timeout_secs = config::kDefaultStreamIdleTimeoutSecs,
                      bool native_web_search = false,
                      nlohmann::json extra_body = nlohmann::json::object(),
                      std::map<std::string, std::string> extra_headers = {},
                      int request_hard_timeout_secs = config::kDefaultRequestHardTimeoutSecs);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

    // 诊断模式的 wire 序列化(与 send_stream 同一条拼装路,见各 client.cpp)。
    std::string SerializeForDiagnostics(const Request& request) const override;

private:
    std::string base_url_;
    std::string auth_token_;
    int connect_timeout_ms_;
    int stream_idle_timeout_secs_;
    bool native_web_search_;
    nlohmann::json extra_body_;
    std::map<std::string, std::string> extra_headers_;
    int request_hard_timeout_secs_;
};

}  // namespace lubancode::api::anthropic
