// QQ v2 富媒体传输(QQ 机器人接入单 Q4:手机发文件,处理后把产物发回去)。
//
// 事实依据(官方 bot.q.qq.com v2 文档,2026-09-15 逐页核读,todo §十 10.1):
//   - 单聊事件 attachments[]:url(下载入口)/filename/size/content_type
//     (枚举含非 MIME 的 "voice"/"file")——url 带 token query,不是可
//     永久依赖的存储,不原样进模型与日志;
//   - 单聊富媒体上传 POST /v2/users/{openid}/files:file_type(1 图片
//     png/jpg、2 视频 mp4、3 语音 silk、4 文件)/url/srv_send_msg/file_name/
//     upload_id;当前页面无 file_data(Base64 直传非官方路径,不走);
//   - 分片预上传 POST /v2/users/{openid}/upload_prepare:请求 file_size
//     为字符串、md5_10m 为前 10002432 字节 MD5;响应 upload_id/block_size
//     (字符串)/parts[]{index,presigned_url,block_size}/upload_config{}
//     {concurrency,retry_timeout,retry_delay}(SDK 1.0.4 从顶层读并发、
//     官方在 upload_config 下——按官方文档页,顶层不认);
//   - 分片完成 POST /v2/users/{openid}/upload_part_finish:{upload_id,
//     part_index,block_size,md5};响应空对象;
//   - 发送 POST /v2/users/{openid}/messages:msg_type=7 + media.file_info
//     (透传,不自己解码);官方示例 msg_type=7 不带 content——文本与
//     附件分段发送,同载荷沿用。
//
// SDK/文档漂移的显式适配(§十 10.1"不能让负偏移进上传循环"):分片偏移
// 按 parts 数组序计算(第 i 片 = 字节 [i*block_size, min((i+1)*block_size,
// file_size))),不按 index 数值——index 从 0 起(官方)或从 1 起(SDK)
// 两种响应都稳定工作,part_index 原值只在 upload_part_finish 回显。
// 真平台核验归 Q3,本地 mock 两案都钉。
//
// 泄露禁令:媒体 url 的 query 带 token/签名——错误文案与诊断一律经
// RedactMediaUrl 脱敏(query 整段折叠),预签名 URL 同款。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_http.hpp"
#include "channel/qq/qq_proto.hpp"

namespace lubancode::channel::qq {

// ---------------------------------------------------------------------------
// 媒体 URL 卫生(纯函数)
// ---------------------------------------------------------------------------

// 脱敏记法:保留 scheme://host/path,query 整段折叠为 "?(redacted)"。
// 无 query 原样返回;解不出 scheme/host 的坏串原样返回(它不是 url)。
std::string RedactMediaUrl(std::string_view url);

// 下载 URL 安全校验:必须 https;host 命中平台后缀白名单(缺省 QQ 域族);
// host 是字面 IP 一律拒(私网/回环绕过与 DNS rebinding 的静态半边);
// authority 带 userinfo 拒。host_allow_suffixes 元素形如 "qq.com"。
std::optional<std::string> ValidateMediaDownloadUrl(
    std::string_view url, const std::vector<std::string>& host_allow_suffixes);

// 缺省平台后缀白名单(QQ 富媒体 CDN 域族;宿主配置可扩)。
const std::vector<std::string>& DefaultMediaHostAllowSuffixes();

// MIME -> 平台 file_type(1/2/3/4)。认不出的按 4(文件)——平台对超软限
// 的图片/视频也会降级为文件上传,文件是最宽的合法档。
int QqFileTypeFromMimeType(std::string_view mime_type);

// ---------------------------------------------------------------------------
// 附件下载(受控 GET;宿主接纳服务调用,产物落受控仓)
// ---------------------------------------------------------------------------

// 出站媒体引用(channel.send 的 file part 折算;宿主 outbox 冻结,适配器
// 读本地原件上传)。
struct QqOutboundMedia {
    std::string local_path;   // 宿主侧 UTF-8 路径(冻结正文/产物原件)
    std::string file_name;    // 展示名(宿主已净化)
    std::string mime_type;
    std::int64_t size_bytes = 0;  // 诊断参考;实际上传以读到的原件为准
};

struct QqMediaDownloadLimits {
    std::int64_t max_bytes = 20 * 1024 * 1024;  // 20 MiB(官方/插件/示例
                                                // 三口径取最小,§十 10.1)
    // 大小帽双重落锤:装配层把同值递给 MakeMediaHttpFunc(传输层在响应
    // 回调入口掐流,大件不进内存),本函数到手后再核实际字节数。
};

struct QqMediaDownloadResult {
    std::string bytes;  // 原件二进制
    // MIME 以事件元数据(attachments[].content_type)为准——传输 seam 不
    // 回响应头,不虚报下载侧观察到的类型。
    std::int64_t size_bytes = 0;
};

// 稳定码:url_insecure | url_host_denied | too_large | not_found |
// platform_error | network_error | invalid_response
struct QqMediaError {
    std::string code;
    std::string detail;  // 脱敏人话(带 url 一律 RedactMediaUrl)
};

// 下载一枚附件。url 经安全校验;GET 不带 QQ Authorization(下载入口的
// 凭据在 url query 里,不进头、不进日志);大小帽双重(Content-Length
// 预检 + 实际字节数复核)。
std::expected<QqMediaDownloadResult, QqMediaError> DownloadQqAttachment(
    const QqHttpFunc& http, const std::string& url,
    const QqMediaDownloadLimits& limits,
    const std::vector<std::string>& host_allow_suffixes = DefaultMediaHostAllowSuffixes());

// ---------------------------------------------------------------------------
// 富媒体上传(分片两步:upload_prepare -> 逐片 PUT -> upload_part_finish
// -> files 合并拿 file_info;发送侧拿 file_info 走 msg_type=7)
// ---------------------------------------------------------------------------

class QqMediaUploader {
public:
    struct Options {
        QqHttpFunc http;
        QqTokenManager* tokens = nullptr;
        std::string api_base = "https://api.sgroup.qq.com";
        std::function<std::int64_t()> now_ms;  // file_info 到期记账
        std::int64_t max_upload_bytes = 20 * 1024 * 1024;  // 宿主产物帽
    };

    struct Outcome {
        enum class Status {
            Uploaded,       // file_info 到手(可发 msg_type=7)
            DeferredRetry,  // 网络/5xx/限频:调用方退避重试
            PermanentFail,  // 格式/大小/容量/未知 4xx:不再自动重试
        };
        Status status = Status::PermanentFail;
        std::string file_info;   // Uploaded 时的透传件
        std::int64_t ttl_secs = 0;
        QqApiError error;        // 非 Uploaded 的分型账
    };

    explicit QqMediaUploader(Options options) : options_(std::move(options)) {}

    // 上传原件拿 file_info。同内容(hash)在 ttl 窗内复用缓存,零网络;
    // 过期重传同一原件(todo §十 10.2——投递重试不重做文件生成)。
    // file_name 应已净化(宿主接纳侧负责);此处再核空名与非空字节。
    Outcome UploadFile(const std::string& openid, const std::string& file_name,
                       const std::string& mime_type, const std::string& bytes);

    // 诊断:file_info 缓存规模。
    std::size_t cached_file_info_count() const;

private:
    Options options_;
    mutable std::mutex mutex_;
    struct CachedFileInfo {
        std::string file_info;
        std::int64_t expires_at_ms = 0;  // 0 = 长期(官方 ttl=0;仍受进程生命周期)
    };
    std::map<std::string, CachedFileInfo> cache_by_sha256_;  // 有界(512)
};

}  // namespace lubancode::channel::qq
