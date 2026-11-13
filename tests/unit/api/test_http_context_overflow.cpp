// 自部署端点 HTTP 超窗识别(Terminal-Bench 自部署单)的单元测试:
// IsHttpContextOverflowBody 的各家报文正反例、ParseContextLimitFromOverflowBody
// 的窗口数解析、TagHttpContextOverflow 补码后与 IsInputContextOverflowCode
// 同表接上。纯函数,不碰网络。

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>

#include "api/model_request_recovery.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

constexpr const char* kVllmBody =
    R"({"object":"error","message":"This model's maximum context length is 32768 tokens. However, you requested 40000 tokens (36000 in the messages, 4000 in the completion). Please reduce the length of the messages or completion.","type":"BadRequestError","param":null,"code":400})";

constexpr const char* kSglangBody =
    R"({"object":"error","message":"The input (40960 tokens) is longer than the model's context length (32768 tokens).","type":"BadRequestError","param":null,"code":400})";

constexpr const char* kLlamaCppBody =
    R"({"error":{"code":400,"message":"the request exceeds the available context size, try increasing it","type":"exceed_context_size_error","n_prompt_tokens":5000,"n_ctx":4096}})";

}  // namespace

TEST_CASE("HTTP 超窗识别: vLLM/SGLang/llama.cpp/Ollama/OpenAI 报文都认") {
    CHECK(api::IsHttpContextOverflowBody(400, kVllmBody));
    CHECK(api::IsHttpContextOverflowBody(400, kSglangBody));
    CHECK(api::IsHttpContextOverflowBody(400, kLlamaCppBody));
    CHECK(api::IsHttpContextOverflowBody(400, R"({"error":{"message":"context length exceeded"}})"));
    CHECK(api::IsHttpContextOverflowBody(400, "Request exceeds the context window of this model"));
    CHECK(api::IsHttpContextOverflowBody(400, R"({"error":"input length exceeds the context length"})"));
    CHECK(api::IsHttpContextOverflowBody(
        400, R"({"error":{"message":"...","type":"invalid_request_error","code":"context_length_exceeded"}})"));
    CHECK(api::IsHttpContextOverflowBody(
        400, R"({"type":"error","error":{"type":"invalid_request_error","message":"prompt is too long: 210000 tokens > 200000 maximum"}})"));
    // 413 同样收。
    CHECK(api::IsHttpContextOverflowBody(413, "Maximum context length exceeded"));
}

TEST_CASE("HTTP 超窗识别: 大小写不敏感") {
    CHECK(api::IsHttpContextOverflowBody(400, "THIS MODEL'S MAXIMUM CONTEXT LENGTH IS 8192 TOKENS"));
    CHECK(api::IsHttpContextOverflowBody(400, "Context Length Exceeded"));
}

TEST_CASE("HTTP 超窗识别: 普通 400 与别的状态码不误判") {
    CHECK_FALSE(api::IsHttpContextOverflowBody(400, R"({"error":{"message":"Invalid value for 'temperature'"}})"));
    CHECK_FALSE(api::IsHttpContextOverflowBody(400, R"({"error":{"message":"model 'foo' not found"}})"));
    CHECK_FALSE(api::IsHttpContextOverflowBody(400, "the context parameter is invalid"));
    CHECK_FALSE(api::IsHttpContextOverflowBody(400, "too many tokens in tool schema name"));
    CHECK_FALSE(api::IsHttpContextOverflowBody(400, ""));
    // 文案对了、状态码不对也不认(500 走瞬时故障重试路,不冒充超窗)。
    CHECK_FALSE(api::IsHttpContextOverflowBody(500, kVllmBody));
    CHECK_FALSE(api::IsHttpContextOverflowBody(429, kVllmBody));
    CHECK_FALSE(api::IsHttpContextOverflowBody(200, kVllmBody));
}

TEST_CASE("超窗报文窗口数解析: 三种写法读出 N") {
    CHECK(api::ParseContextLimitFromOverflowBody(kVllmBody) == std::optional<std::size_t>(32768));
    CHECK(api::ParseContextLimitFromOverflowBody(kSglangBody) == std::optional<std::size_t>(32768));
    CHECK(api::ParseContextLimitFromOverflowBody(kLlamaCppBody) == std::optional<std::size_t>(4096));
    // 千分位与大小写。
    CHECK(api::ParseContextLimitFromOverflowBody("Maximum Context Length Is 131,072 tokens") ==
          std::optional<std::size_t>(131072));
}

TEST_CASE("超窗报文窗口数解析: 读不出正整数给空") {
    CHECK_FALSE(api::ParseContextLimitFromOverflowBody("context length exceeded").has_value());
    CHECK_FALSE(api::ParseContextLimitFromOverflowBody("maximum context length is unknown").has_value());
    CHECK_FALSE(api::ParseContextLimitFromOverflowBody("maximum context length is 0 tokens").has_value());
    CHECK_FALSE(api::ParseContextLimitFromOverflowBody("").has_value());
    // 数字超长(溢出)不采用。
    CHECK_FALSE(api::ParseContextLimitFromOverflowBody("maximum context length is 99999999999999999999999 tokens")
                    .has_value());
}

TEST_CASE("超窗补码: HttpStatus 超窗补 context_length_exceeded,接上现有超窗表") {
    api::Error error{api::ErrorKind::HttpStatus, kVllmBody, 400};
    api::TagHttpContextOverflow(error);
    CHECK(error.api_code == "context_length_exceeded");
    CHECK(api::IsInputContextOverflowCode(error.api_code));
    // 补码不改变重试判定:400 照旧不可重试(重发同一份输入只会再被拒)。
    CHECK_FALSE(api::IsRetryableError(error));
    CHECK(api::ReasonCodeOfError(error) == "http.400");
}

TEST_CASE("超窗补码: 普通 400、非 HttpStatus、已带码的错误原样不动") {
    api::Error plain{api::ErrorKind::HttpStatus, R"({"error":{"message":"invalid tool schema"}})", 400};
    api::TagHttpContextOverflow(plain);
    CHECK(plain.api_code.empty());

    api::Error network{api::ErrorKind::Network, "maximum context length is 8192", 0};
    api::TagHttpContextOverflow(network);
    CHECK(network.api_code.empty());

    api::Error tagged{api::ErrorKind::HttpStatus, kVllmBody, 400};
    tagged.api_code = "something_else";
    api::TagHttpContextOverflow(tagged);
    CHECK(tagged.api_code == "something_else");
}
