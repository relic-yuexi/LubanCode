// Lua 受控 HTTP 的宿主侧合同(阶段 0:冻结合同与可替换 seam)。
//
// 这一层只放四样东西:§11 的稳定错误码总表、URL 规范化纯函数、
// BoundedHttpTransport 接口与 fake。真传输(cpr/libcurl、字节帽、取消)
// 是阶段 2 的事;Lua 注册(luban.http/luban.secrets、SecretRef userdata、
// LuaCallContext)是阶段 3 的事。
//
// ---------------------------------------------------------------------------
// Lua Host API 表形(§六,注释合同——实现见阶段 3):
//
//   luban.http.request{
//     method  = "GET"|"POST",            -- 必填,须命中 manifest methods
//     url     = "https://host/path",     -- 必填,绝对 HTTPS,禁 userinfo/fragment
//     headers = { [name] = value },      -- 可选;禁 Authorization/Proxy-
//                                         -- Authorization/Cookie/Host/
//                                         -- Content-Length(宿主代填的归宿主)
//     json    = <table>,                 -- 与 body 二选一
//     body    = "<utf-8>",               -- 与 json 二选一
//     auth    = { type = "bearer"|"header", secret = "<逻辑 id>",
//                 optional = bool, name = "<header 名>", prefix = "<前缀>" },
//     timeout_ms = <int>,                -- 只能下调 manifest 与宿主上限
//   } -> response|nil, err
//
//   成功 response: { status, headers, body, json?, url, bytes }
//   失败 err:      { code = "<稳定码>", message, status = 0, retryable }
//   失败不抛 C++ 异常穿过 Lua 边界;HTTP 非 2xx 不混作网络错(§11)。
//
//   luban.secrets.available("id") -> bool
//   luban.secrets.ref("id")       -> opaque userdata(tostring 只得
//                                     <secret:id>;不可拼接/索引/转 JSON;
//                                     唯一 sink 是 request.auth)
//
// 顶层零副作用规矩(§九,注释合同——阶段 3 以 LuaCallContext RAII 落地):
//   1. 建 state、开 Pure 库、注册 luban 模块;
//   2. 加载并执行 chunk 时 LuaCallContext 为空;
//   3. 顶层调 HTTP/Secret 返回 no_active_tool_call,零网络、零 Secret 解析;
//   4. 验 handler 表与 manifest entry;
//   5. 工具调用前用 RAII 把 LuaCallContext* 写进 registry;
//   6. lua_pcall 返回后立刻清空;异常与取消路径也须清空。
//   验收:假 resolver/transport 计数器在恶意顶层脚本下仍为零。
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/plugin_contract.hpp"  // NetworkPermission/HttpLimits 等

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 宿主错误码总表(§11 全表)。值即 ABI:只增不改;Lua 与测试都按
// CodeName() 的稳定串判断,不解析文案。
// ---------------------------------------------------------------------------
enum class LuaHostErrorCode {
    NoActiveToolCall = 1,   // no_active_tool_call:顶层加载期或调用外使用 Host API
    NetworkNotDeclared,     // network_not_declared:manifest 未声明网络
    NetworkTargetDenied,    // network_target_denied:scheme/host/port/method 不匹配
    NetworkAddressDenied,   // network_address_denied:DNS 落私网/保留地址或连接地址不可信
    SecretNotDeclared,      // secret_not_declared:Lua 引用了未声明 Secret
    SecretMissing,          // secret_missing:required Secret 没找到
    InvalidRequest,         // invalid_request:URL/header/body/auth 形状错
    RequestTooLarge,        // request_too_large:URL/header/body 超帽
    ResponseTooLarge,       // response_too_large:响应头/体超帽
    Timeout,                // timeout:宿主墙钟到点
    Cancelled,              // cancelled:ESC/父任务取消
    DnsFailed,              // dns_failed:解析失败
    TlsFailed,              // tls_failed:TLS/证书失败
    NetworkFailed,          // network_failed:其它传输错误
    HttpStatus,             // http_status:HTTP 已到达,status 原样带回
    InvalidJson,            // invalid_json:请求 JSON 序列化或响应 JSON 解析失败
    ConcurrencyLimit,       // concurrency_limit:在途请求超宿主上限
};

// 稳定串(§11 表的 code 列)。
std::string_view LuaHostErrorCodeName(LuaHostErrorCode code);

// 缺省 retryable(§11 表)。tls_failed 与 http_status 的重试判断有上下文
// (证书类 false、连接类视错误;408/429/5xx 由 Lua 定),这里给缺省值,
// transport 层可按分型覆盖。
bool LuaHostErrorCodeDefaultRetryable(LuaHostErrorCode code);

// 缺省人话(不含 Secret 值、header 全文、请求体与 .env 原文——§11 的
// 文案禁令)。调用方可在其上补上下文,但补的内容同样过禁令。
std::string LuaHostErrorCodeDefaultMessage(LuaHostErrorCode code);

// ---------------------------------------------------------------------------
// URL 规范化(纯函数,不接网;§8.2 第 1/3 步的静态半边)。
//   - 只收绝对 http/https URL;scheme/host 小写、host 去末尾点、IDNA 规
//     谱化(与 manifest 声明共用 NormalizeDnsHost)。
//   - 禁 userinfo(user@host)与 fragment(#...):InvalidRequest。
//   - host 是 IP 字面量、localhost、.local:NetworkAddressDenied。
//   - path 空 → "/";query 不带 '?'。端口显式给出才记账(默认端口按
//     scheme 推:https=443)。
// ---------------------------------------------------------------------------
struct NormalizedUrl {
    std::string scheme;    // 小写("https"/"http")
    std::string host;      // 规范化 DNS 名
    int port = 0;          // 显式或默认(https=443,http=80)
    bool has_explicit_port = false;
    std::string path;      // "/" 起头
    std::string query;     // 不带 '?';空 = 无
    std::string text;      // 规范化后的全串(scheme://host[:port]path[?query])
};

std::expected<NormalizedUrl, LuaHostErrorCode> NormalizeHttpUrl(std::string_view raw_url);

// URL 与 manifest 网络账的对账(纯函数):scheme/host/port 全中才放行。
// method 不在本表(方法对账在请求形状层);path/query 由 Lua 决定。
bool NetworkPermissionAllows(const std::vector<NetworkPermission>& permissions, const NormalizedUrl& url);

// ---------------------------------------------------------------------------
// BoundedHttpTransport:受控非流式 HTTP 的可替换 seam(§8)。阶段 0 只有
// 接口与 fake;真传输(中立 cpr 底座 + 字节帽落锤 + 三段取消)是阶段 2。
// 接口同步:一个 Lua tool call 同时只跑一笔 HTTP(§8.5)。
// ---------------------------------------------------------------------------
struct HttpExchangeRequest {
    std::string method;  // "GET"/"POST"
    std::string url;     // 已规范化的绝对 URL
    // 有序头表(可重复;Secret 注入后的最终头由宿主在发包前一刻拼好,
    // 拼好的表不进公共日志——§5.4/§7.4)。
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;    // 可空
};

struct HttpExchangeResponse {
    int status = 0;
    // 重复头按数组形状保留(§6.2);敏感/无用字段(set-cookie、
    // proxy-authenticate 一类)在传输层过滤后进表。
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string final_url;  // 第一版不跟重定向,同请求 URL
};

// 传输错误:§11 分型 + 人话(文案过 §11 的禁令:不含完整敏感 header、
// Secret 明文/长度/前后缀、请求体全文、.env 原文)。
struct HttpTransportError {
    LuaHostErrorCode code = LuaHostErrorCode::NetworkFailed;
    std::string message;
    int status = 0;  // code=HttpStatus 时有效,原样带回
};

class BoundedHttpTransport {
public:
    virtual ~BoundedHttpTransport() = default;

    // 执行一笔受控请求。limits 是已生效的帽(宿主硬帽与 manifest 下调值
    // 的合成,由调用方算好递进来);cancel 覆盖连接前/DNS/等首字节/收体
    // 全程(§8.4);超帽即中止,不先攒完整响应(§8.3)。
    virtual std::expected<HttpExchangeResponse, HttpTransportError> Execute(
        const HttpExchangeRequest& request, const EffectiveHttpLimits& limits,
        const std::atomic<bool>* cancel) = 0;
};

// Fake:测试用。记账收到的请求(验"越权不发包"一类断言),按编排好的
// 脚本回响应或错误;不动网。
class FakeHttpTransport final : public BoundedHttpTransport {
public:
    struct Call {
        HttpExchangeRequest request;
        EffectiveHttpLimits limits;
        bool cancel_observed = false;
    };

    std::expected<HttpExchangeResponse, HttpTransportError> Execute(
        const HttpExchangeRequest& request, const EffectiveHttpLimits& limits,
        const std::atomic<bool>* cancel) override;

    // 编排下一笔的结果(先到先得;耗尽后返回 NetworkFailed)。
    void EnqueueResponse(HttpExchangeResponse response);
    void EnqueueError(HttpTransportError error);

    const std::vector<Call>& calls() const { return calls_; }
    std::size_t call_count() const { return calls_.size(); }
    void ClearScript();

private:
    struct Scripted {
        bool is_error = false;
        HttpExchangeResponse response;
        HttpTransportError error;
    };
    std::vector<Call> calls_;
    std::vector<Scripted> script_;
};

}  // namespace lubancode::runtime
