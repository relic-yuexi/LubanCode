// 中立 HTTP 传输底座(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 2)。
//
// 设计单 §8.1:抽一层不认任何业务语义的传输件——plugin HTTP(受控非流式)
// 先用;provider SSE 路暂不迁(回归钉稳是后话,一次别改四家 wire)。
// 本层只做四件事:
//   - GET/POST 一笔完整响应(非流式,响应攒齐再回);
//   - 连接超时 + 硬墙钟(ProgressCallback 落锤,分型 timeout);
//   - Progress/Write 双回调取消(覆盖连接/上传/等首字节/收体全程);
//   - 请求/响应字节记账(响应头/响应体在回调入口落帽,不先攒完再看)。
//
// 纪律:本层不认 plugin/manifest/Lua 概念,不 include 任何 runtime 头;
// 不打日志——最终头表里可能有 Secret,谁也不许在这里留痕。
// URL 规范化与权限对账在调用方(runtime/plugin_http),这里只收已定形的
// 绝对 URL。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::net {

// ---------------------------------------------------------------------------
// 请求/响应形状(中立:有序头表,可重复)。
// ---------------------------------------------------------------------------
struct FullHttpRequest {
    std::string method;  // "GET"/"POST"(大写;别的由调用方拦)
    std::string url;     // 绝对 URL(调用方已规范化)
    // 最终头表(Secret 注入后的样子)。可重复;本层原样发,不猜语义。
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;    // 可空;GET 不该带体(调用方拦)
};

// 连接期钉地址(§8.2 第 5 步,防 DNS rebinding):host:port 钉到已验 addr,
// libcurl 走 CURLOPT_RESOLVE 的缓存条目,不再自行动 DNS。
struct PinnedDnsResolve {
    std::string host;
    int port = 0;
    std::string addr;   // IP 串(v4 点分或 v6 裸串)
};

// 传输层落锤的帽(§8.3 表的传输侧半边)。URL/请求头/请求体帽在调用方
// (cpr 前);这里的四项全在传输回调入口执行。
struct FullHttpLimits {
    std::int64_t connect_timeout_ms = 10'000;   // 连接阶段上限
    std::int64_t hard_timeout_ms = 30'000;      // 硬墙钟(全程)
    std::int64_t response_header_bytes = 64 * 1024;  // 响应头块字节(含状态行与 CRLF)
    std::int64_t response_body_bytes = 4 * 1024 * 1024;  // 响应体字节
};

struct FullHttpResponse {
    int status = 0;
    // 按到达顺序保留的响应头(重复头不并);状态行不在表里。名字保持
    // 原样大小写——过滤/小写化是调用方(runtime 层)的合同。
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// 传输错误分型。Cancelled/Timeout/Response*TooLarge 是我们自己掐的流
//(回调返回 false),kind 即终判;其余看 curl 错误码(curl_code/curl_message
// 原样带回,由调用方映射自家错误合同)。
enum class FullHttpErrorKind {
    Cancelled = 1,
    Timeout,
    ResponseHeaderTooLarge,
    ResponseBodyTooLarge,
    DnsFailed,
    TlsFailed,
    NetworkFailed,
};

struct FullHttpError {
    FullHttpErrorKind kind = FullHttpErrorKind::NetworkFailed;
    std::string message;   // 人话;不含请求头/体(§11 文案禁令)
    long curl_code = 0;    // CURLE_*(cpr::ErrorCode 的底层值);0 = 无
    std::string curl_message;
    bool received_any_bytes = false;  // 收到过响应体字节(超时分型旁证)
};

// 执行一笔完整请求。同步阻塞;cancel 非空且置位时在连接/上传/等首字节/
// 收体任一阶段就地掐流,分型 Cancelled(§8.4)。响应头/体在回调入口记账,
// 达帽即中止,不先攒完整响应(§8.3)。
std::expected<FullHttpResponse, FullHttpError> PerformFullHttpRequest(
    const FullHttpRequest& request, const FullHttpLimits& limits, const std::atomic<bool>* cancel,
    const PinnedDnsResolve* pinned);

// cpr/libcurl 错误码 -> 传输错误分型(纯函数,单测直钉全表)。curl_code
// 是 cpr::ErrorCode 的底层值(CURLE_*)。OPERATION_TIMEDOUT 归 Timeout;
// 我们自己掐的流(取消/帽)不走这里。
FullHttpErrorKind ClassifyCurlErrorCode(long curl_code);

// ---------------------------------------------------------------------------
// IP 地址解析与安全分类(纯函数,不接网)。§8.2 第 4 步的静态半边:
// DNS 候选地址落 loopback/link-local/RFC1918/CGNAT/组播/保留段/云
// metadata 段的一律不放行。
// ---------------------------------------------------------------------------
struct ParsedAddress {
    bool is_ipv4 = false;                    // v6 的 v4 映射段仍算 v6 字节形态
    std::array<std::uint8_t, 16> bytes{};    // v4 只占前 4 字节
};

// IPv4 点分 / IPv6(含 "::" 压缩、内嵌 v4 尾巴、"%zone" 剥离)。失败返回
// nullopt,不猜。
std::optional<ParsedAddress> ParseIpAddress(std::string_view text);

// 落禁连段时返回段名(人话,进错误文案);公网地址返回 nullopt。
std::optional<std::string> BlockedAddressRange(std::string_view ip);

// 便利谓词:解析得动且不在任何禁连段。
bool IsRoutablePublicAddress(std::string_view ip);

// loopback 判定(v4 127.0.0.0/8 与 v6 ::1)。受控 HTTP 的测试口
// (本机假服务)与诊断展示共用;不是放行令——放行与否仍是调用方的账。
bool IsLoopbackAddress(std::string_view ip);

// ---------------------------------------------------------------------------
// DNS 解析 seam(§8.2 第 4/5 步的可替换口)。生产走 SystemDnsResolver
// (getaddrinfo);测试注入假账,不碰网。
// ---------------------------------------------------------------------------
class DnsResolver {
public:
    virtual ~DnsResolver() = default;

    // 解析一枚 DNS 名,按系统给出顺序返回候选地址串(v4 点分/v6 裸串)。
    // 失败返回人话(DnsFailed 文案用)。不做任何过滤——分类是调用方的账。
    virtual std::expected<std::vector<std::string>, std::string> Resolve(const std::string& host) = 0;
};

class SystemDnsResolver final : public DnsResolver {
public:
    SystemDnsResolver() = default;

    std::expected<std::vector<std::string>, std::string> Resolve(const std::string& host) override;
};

}  // namespace lubancode::net
