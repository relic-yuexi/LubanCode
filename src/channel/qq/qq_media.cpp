// QQ v2 富媒体传输实现(QQ 接入单 Q4)。合同见 qq_media.hpp;协议纯函数
// 在 qq_proto(请求/响应形状与宽松解析)。
//
// MD5/SHA-1 内核:上传接口必填校验值(md5/sha1/md5_10m,官方字段表),
// 公有域标准算法(RFC 1321 / FIPS 180-1)的标准写法,只做完整性校验,
// 不当密码学防线。已知测试向量在 test_qq_media.cpp 钉死。
#include "channel/qq/qq_media.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "net/http_transport.hpp"  // ParseIpAddress:字面 IP 拒(静态半边)
#include "platform/sha256.hpp"

namespace lubancode::channel::qq {

namespace {

// ---------------------------------------------------------------------------
// MD5(RFC 1321)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr std::uint32_t kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

std::uint32_t RotateLeft(std::uint32_t value, std::uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::string Md5Hex(const std::string& data) {
    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    const auto process_block = [&](const std::uint8_t* block) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
        }
        std::uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::uint32_t g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = static_cast<std::uint32_t>(i);
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5u * static_cast<std::uint32_t>(i) + 1u) % 16u;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3u * static_cast<std::uint32_t>(i) + 5u) % 16u;
            } else {
                f = c ^ (b | ~d);
                g = (7u * static_cast<std::uint32_t>(i)) % 16u;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + RotateLeft(a + f + kMd5K[i] + m[g], kMd5S[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    std::vector<std::uint8_t> padded(data.begin(), data.end());
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    for (int i = 0; i < 8; ++i) {
        padded.push_back(static_cast<std::uint8_t>((bit_len >> (8 * i)) & 0xff));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        process_block(padded.data() + offset);
    }

    std::string out;
    out.reserve(32);
    const std::uint32_t words[4] = {a0, b0, c0, d0};
    char hex[3];
    for (const std::uint32_t word : words) {
        for (int byte = 0; byte < 4; ++byte) {
            std::snprintf(hex, sizeof(hex), "%02x",
                          static_cast<unsigned>((word >> (8 * byte)) & 0xff));
            out.append(hex, 2);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// SHA-1(FIPS 180-1)
// ---------------------------------------------------------------------------

std::string Sha1Hex(const std::string& data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const auto process_block = [&](const std::uint8_t* block) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };

    std::vector<std::uint8_t> padded(data.begin(), data.end());
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        padded.push_back(static_cast<std::uint8_t>((bit_len >> (8 * i)) & 0xff));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        process_block(padded.data() + offset);
    }

    std::string out;
    out.reserve(40);
    char hex[3];
    for (const std::uint32_t word : h) {
        // big-endian 字节序:24/16/8/0(28 起会跨字节边界取半字节——
        // 首轮 CI 红的根因,标准向量钉死)。
        for (int shift = 24; shift >= 0; shift -= 8) {
            std::snprintf(hex, sizeof(hex), "%02x",
                          static_cast<unsigned>((word >> shift) & 0xff));
            out.append(hex, 2);
        }
    }
    return out;
}

// 前 limit 字节的 MD5(md5_10m 用;官方"前 10002432 字节(约 10MB)")。
std::string Md5HexOfPrefix(const std::string& data, std::int64_t limit) {
    const std::int64_t size = static_cast<std::int64_t>(data.size());
    const std::int64_t take = size < limit ? size : limit;
    return Md5Hex(data.substr(0, static_cast<std::size_t>(take)));
}

// ---------------------------------------------------------------------------
// URL 卫生
// ---------------------------------------------------------------------------

std::string ToLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace

std::string RedactMediaUrl(std::string_view url) {
    // query 整段折叠(签名/token 都在 query);fragment 一并折叠。
    const std::size_t cut = url.find_first_of("?#");
    if (cut == std::string_view::npos) {
        return std::string(url);
    }
    std::string out(url.substr(0, cut));
    out += url[cut] == '?' ? "?(redacted)" : "#(redacted)";
    return out;
}

std::optional<std::string> ValidateMediaDownloadUrl(
    std::string_view url, const std::vector<std::string>& host_allow_suffixes) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return "url_insecure: missing scheme";
    }
    const std::string scheme = ToLowerAscii(url.substr(0, scheme_end));
    if (scheme != "https") {
        return "url_insecure: scheme is not https";
    }
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t authority_end =
        url.find_first_of("/?#", authority_start);
    const std::string authority =
        std::string(url.substr(authority_start, authority_end == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : authority_end - authority_start));
    if (authority.empty()) {
        return "url_insecure: empty host";
    }
    if (authority.find('@') != std::string::npos) {
        return "url_insecure: userinfo in authority";
    }
    std::string host = authority;
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos) {
        // 带端口:截 host(多冒号 = IPv6 字面量,下一道字面 IP 检查拒)。
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        return "url_insecure: empty host";
    }
    // 字面 IP 一律拒(私网/回环/组播绕过的静态半边;平台域不该是裸 IP)。
    if (net::ParseIpAddress(host).has_value()) {
        return "url_host_denied: literal ip host";
    }
    host = ToLowerAscii(host);
    for (const std::string& suffix : host_allow_suffixes) {
        if (host == suffix) {
            return std::nullopt;
        }
        if (host.size() > suffix.size() + 1) {
            const std::string tail = host.substr(host.size() - suffix.size() - 1);
            if (tail == "." + suffix) {
                return std::nullopt;
            }
        }
    }
    return "url_host_denied: host not in platform allowlist";
}

const std::vector<std::string>& DefaultMediaHostAllowSuffixes() {
    // QQ 富媒体 CDN 域族(官方事件 url 的观察域;宿主配置可扩)。
    static const std::vector<std::string> kSuffixes = {
        "qq.com", "qq.com.cn", "qpic.cn", "myqcloud.com", "tencent-cloud.com",
    };
    return kSuffixes;
}

int QqFileTypeFromMimeType(std::string_view mime_type) {
    if (mime_type.rfind("image/", 0) == 0) {
        return 1;
    }
    if (mime_type.rfind("video/", 0) == 0 || mime_type == "video/mp4") {
        return 2;
    }
    if (mime_type == "voice" || mime_type == "audio/silk" || mime_type == "audio/amr" ||
        mime_type.rfind("audio/", 0) == 0) {
        return 3;
    }
    return 4;
}

std::expected<QqMediaDownloadResult, QqMediaError> DownloadQqAttachment(
    const QqHttpFunc& http, const std::string& url, const QqMediaDownloadLimits& limits,
    const std::vector<std::string>& host_allow_suffixes) {
    if (const auto blocked = ValidateMediaDownloadUrl(url, host_allow_suffixes)) {
        return std::unexpected(QqMediaError{*blocked, RedactMediaUrl(url)});
    }
    QqHttpRequest request;
    request.method = "GET";
    request.url = url;
    // 不带 QQ Authorization:下载凭据在 url query(平台签名入口),不进头
    // 也不进任何日志(错误文案经 RedactMediaUrl)。
    const auto response = http(request);
    if (!response.has_value()) {
        return std::unexpected(QqMediaError{"network_error", response.error()});
    }
    if (response->status == 404) {
        return std::unexpected(QqMediaError{"not_found", RedactMediaUrl(url)});
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(
            QqMediaError{"platform_error",
                         "status " + std::to_string(response->status) + " url " +
                             RedactMediaUrl(url)});
    }
    if (response->body.empty()) {
        return std::unexpected(QqMediaError{"invalid_response", "empty body"});
    }
    if (static_cast<std::int64_t>(response->body.size()) > limits.max_bytes) {
        return std::unexpected(
            QqMediaError{"too_large",
                         std::to_string(response->body.size()) + " bytes over cap " +
                             std::to_string(limits.max_bytes)});
    }
    QqMediaDownloadResult out;
    out.bytes = std::move(response->body);
    out.size_bytes = static_cast<std::int64_t>(out.bytes.size());
    return out;
}

// ---------------------------------------------------------------------------
// QqMediaUploader
// ---------------------------------------------------------------------------

namespace {

// 媒体错误折算 DeferredRetry/PermanentFail(与 QqMessageSender 的发送分型
// 同一把尺:可重试族只有 RateLimited/ServerError/NetworkError)。
QqMediaUploader::Outcome::Status DeferredOrPermanent(const QqApiError& error) {
    switch (error.kind) {
        case QqApiErrorKind::RateLimited:
        case QqApiErrorKind::ServerError:
        case QqApiErrorKind::NetworkError:
            return QqMediaUploader::Outcome::Status::DeferredRetry;
        default:
            return QqMediaUploader::Outcome::Status::PermanentFail;
    }
}

// 平台错误体(2xx + {"code":..})检查:非 0 code 返回分型,nullopt = 无错。
std::optional<QqApiError> ParsePlatformError(int status, const std::string& body) {
    const auto parsed =
        nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("code")) {
        return std::nullopt;
    }
    if (ParseLooseInt64(parsed.at("code")).value_or(0) == 0) {
        return std::nullopt;
    }
    return ClassifyQqSendFailure(status, body);
}

}  // namespace

std::size_t QqMediaUploader::cached_file_info_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_by_sha256_.size();
}

QqMediaUploader::Outcome QqMediaUploader::UploadFile(const std::string& openid,
                                                     const std::string& file_name,
                                                     const std::string& mime_type,
                                                     const std::string& bytes) {
    Outcome outcome;
    if (bytes.empty() || file_name.empty()) {
        outcome.error.kind = QqApiErrorKind::ContentRejected;
        outcome.error.detail = "upload rejected: empty file or name";
        return outcome;
    }
    if (static_cast<std::int64_t>(bytes.size()) > options_.max_upload_bytes) {
        outcome.error.kind = QqApiErrorKind::ContentRejected;
        outcome.error.detail =
            "upload over cap: " + std::to_string(bytes.size()) + " bytes";
        return outcome;
    }
    const std::string content_key = platform::Sha256Hex(bytes);
    const std::int64_t now = options_.now_ms ? options_.now_ms() : 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = cache_by_sha256_.find(content_key);
        if (cached != cache_by_sha256_.end() &&
            (cached->second.expires_at_ms == 0 || now < cached->second.expires_at_ms)) {
            // file_info 未过期:复用,零网络(§十 10.2"过期重传同一原件,
            // 投递失败不重做文件生成")。
            outcome.status = Outcome::Status::Uploaded;
            outcome.file_info = cached->second.file_info;
            outcome.ttl_secs = cached->second.expires_at_ms == 0
                                   ? 0
                                   : (cached->second.expires_at_ms - now) / 1000;
            return outcome;
        }
        if (cached != cache_by_sha256_.end()) {
            cache_by_sha256_.erase(cached);  // 过期:重传同一原件
        }
    }

    const int file_type = QqFileTypeFromMimeType(mime_type);
    const std::string whole_md5 = Md5Hex(bytes);
    const std::string whole_sha1 = Sha1Hex(bytes);
    const std::string md5_10m = Md5HexOfPrefix(bytes, 10'002'432);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto token = options_.tokens->GetValidToken();
        if (!token.has_value()) {
            QqApiError error;
            error.kind = QqApiErrorKind::NetworkError;
            switch (token.error().kind) {
                case QqTokenManager::ErrorKind::RateLimited:
                    error.kind = QqApiErrorKind::RateLimited;
                    break;
                case QqTokenManager::ErrorKind::ServerError:
                    error.kind = QqApiErrorKind::ServerError;
                    break;
                default:
                    break;
            }
            error.detail = "token: " + token.error().detail;
            outcome.status = DeferredOrPermanent(error);
            outcome.error = std::move(error);
            return outcome;
        }

        // 1) upload_prepare。
        QqHttpRequest prepare_request;
        prepare_request.method = "POST";
        prepare_request.url = options_.api_base + UploadPreparePath(openid);
        prepare_request.headers.emplace_back("Content-Type", "application/json");
        prepare_request.headers.emplace_back("Authorization", "QQBot " + *token);
        prepare_request.body =
            BuildUploadPrepareRequest(file_type, static_cast<std::int64_t>(bytes.size()),
                                      file_name, whole_md5, whole_sha1, md5_10m)
                .dump();
        const auto prepare_response = options_.http(prepare_request);
        if (!prepare_response.has_value()) {
            outcome.error.kind = QqApiErrorKind::NetworkError;
            outcome.error.detail = "upload_prepare: " + prepare_response.error();
            outcome.status = Outcome::Status::DeferredRetry;
            return outcome;
        }
        if (const auto platform_error = ParsePlatformError(
                prepare_response->status, prepare_response->body)) {
            if (platform_error->kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                options_.tokens->Invalidate();
                continue;
            }
            outcome.status = DeferredOrPermanent(*platform_error);
            outcome.error = *platform_error;
            return outcome;
        }
        if (prepare_response->status < 200 || prepare_response->status >= 300) {
            QqApiError error =
                ClassifyQqSendFailure(prepare_response->status, prepare_response->body);
            if (error.kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                options_.tokens->Invalidate();
                continue;
            }
            outcome.status = DeferredOrPermanent(error);
            outcome.error = std::move(error);
            return outcome;
        }
        const auto prepare_json = nlohmann::json::parse(
            prepare_response->body, nullptr, /*allow_exceptions=*/false);
        std::string parse_error;
        const auto prepared = ParseUploadPrepareResponse(prepare_json, &parse_error);
        if (!prepared.has_value()) {
            outcome.error.kind = QqApiErrorKind::InvalidResponse;
            outcome.error.detail = "upload_prepare: " + parse_error;
            outcome.status = Outcome::Status::PermanentFail;
            return outcome;
        }

        // 2) 逐片 PUT(按 parts 数组序取偏移,不按 index 值——官方从 0 起、
        //    SDK 漂移从 1 起,数组序对两案都稳;§十 10.1)。
        const std::int64_t total = static_cast<std::int64_t>(bytes.size());
        bool unauthorized_retry = false;
        for (std::size_t i = 0; i < prepared->parts.size(); ++i) {
            const UploadPreparePart& part = prepared->parts[i];
            const std::int64_t offset =
                static_cast<std::int64_t>(i) * prepared->block_size;
            const std::int64_t length =
                (total - offset) < prepared->block_size ? (total - offset)
                                                        : prepared->block_size;
            if (offset >= total || length <= 0) {
                outcome.error.kind = QqApiErrorKind::InvalidResponse;
                outcome.error.detail = "upload parts overrun file size";
                outcome.status = Outcome::Status::PermanentFail;
                return outcome;
            }
            QqHttpRequest put_request;
            put_request.method = "PUT";
            put_request.url = part.presigned_url;
            put_request.headers.emplace_back("Content-Type", "application/octet-stream");
            // 预签名 URL 自带鉴权(COS 签名),不带 QQ Authorization
            //(§十 10.1:COS PUT 不带 QQ Authorization)。
            put_request.body = bytes.substr(static_cast<std::size_t>(offset),
                                            static_cast<std::size_t>(length));
            const auto put_response = options_.http(put_request);
            if (!put_response.has_value()) {
                outcome.error.kind = QqApiErrorKind::NetworkError;
                outcome.error.detail = "upload part put: " + put_response.error();
                outcome.status = Outcome::Status::DeferredRetry;
                return outcome;
            }
            if (put_response->status < 200 || put_response->status >= 300) {
                // 预签名失败(403 过期/签名不符)是永久性的:不刷 token 重试。
                outcome.error.kind = put_response->status >= 500
                                         ? QqApiErrorKind::ServerError
                                         : QqApiErrorKind::UnknownError;
                outcome.error.http_status = put_response->status;
                outcome.error.detail = "upload part put status " +
                                        std::to_string(put_response->status) + " url " +
                                        RedactMediaUrl(part.presigned_url);
                outcome.status = DeferredOrPermanent(outcome.error);
                return outcome;
            }

            // 3) upload_part_finish(该片完成回执;part_index 回显平台原值)。
            QqHttpRequest finish_request;
            finish_request.method = "POST";
            finish_request.url = options_.api_base + UploadPartFinishPath(openid);
            finish_request.headers.emplace_back("Content-Type", "application/json");
            finish_request.headers.emplace_back("Authorization", "QQBot " + *token);
            finish_request.body =
                BuildUploadPartFinishRequest(prepared->upload_id, part.index, length,
                                             Md5Hex(put_request.body))
                    .dump();
            const auto finish_response = options_.http(finish_request);
            if (!finish_response.has_value()) {
                outcome.error.kind = QqApiErrorKind::NetworkError;
                outcome.error.detail = "upload part finish: " + finish_response.error();
                outcome.status = Outcome::Status::DeferredRetry;
                return outcome;
            }
            if (const auto platform_error = ParsePlatformError(finish_response->status,
                                                               finish_response->body)) {
                if (platform_error->kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                    unauthorized_retry = true;
                    break;
                }
                outcome.status = DeferredOrPermanent(*platform_error);
                outcome.error = *platform_error;
                return outcome;
            }
            if (finish_response->status < 200 || finish_response->status >= 300) {
                QqApiError error = ClassifyQqSendFailure(finish_response->status,
                                                         finish_response->body);
                if (error.kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                    unauthorized_retry = true;
                    break;
                }
                outcome.status = DeferredOrPermanent(error);
                outcome.error = std::move(error);
                return outcome;
            }
        }
        if (unauthorized_retry) {
            options_.tokens->Invalidate();
            continue;  // 刷 token 后整装重来(upload_prepare 重发无害)
        }

        // 4) files 合并拿 file_info(srv_send_msg=false:发送另走 messages)。
        QqHttpRequest merge_request;
        merge_request.method = "POST";
        merge_request.url = options_.api_base + FileUploadPath(openid);
        merge_request.headers.emplace_back("Content-Type", "application/json");
        merge_request.headers.emplace_back("Authorization", "QQBot " + *token);
        merge_request.body =
            BuildFileUploadBody(file_type, file_name, prepared->upload_id).dump();
        const auto merge_response = options_.http(merge_request);
        if (!merge_response.has_value()) {
            outcome.error.kind = QqApiErrorKind::NetworkError;
            outcome.error.detail = "upload merge: " + merge_response.error();
            outcome.status = Outcome::Status::DeferredRetry;
            return outcome;
        }
        if (const auto platform_error =
                ParsePlatformError(merge_response->status, merge_response->body)) {
            if (platform_error->kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                options_.tokens->Invalidate();
                continue;
            }
            outcome.status = DeferredOrPermanent(*platform_error);
            outcome.error = *platform_error;
            return outcome;
        }
        if (merge_response->status < 200 || merge_response->status >= 300) {
            QqApiError error =
                ClassifyQqSendFailure(merge_response->status, merge_response->body);
            if (error.kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                options_.tokens->Invalidate();
                continue;
            }
            outcome.status = DeferredOrPermanent(error);
            outcome.error = std::move(error);
            return outcome;
        }
        const auto merge_json =
            nlohmann::json::parse(merge_response->body, nullptr, /*allow_exceptions=*/false);
        const auto merged = ParseFileUploadResponse(merge_json, &parse_error);
        if (!merged.has_value()) {
            outcome.error.kind = QqApiErrorKind::InvalidResponse;
            outcome.error.detail = "upload merge: " + parse_error;
            outcome.status = Outcome::Status::PermanentFail;
            return outcome;
        }
        // 缓存有界(512);过期时刻 = ttl*1000(ttl=0 官方语义"长期",记 0 =
        // 不过期检查;ttl 缺失 = -1 按"已过期"处理,下次重传,不虚报长期)。
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (cache_by_sha256_.size() >= 512) {
                cache_by_sha256_.erase(cache_by_sha256_.begin());
            }
            const std::int64_t expires_at =
                merged->ttl_secs < 0 ? now
                                     : (merged->ttl_secs == 0 ? 0
                                                              : now + merged->ttl_secs * 1000);
            cache_by_sha256_[content_key] = CachedFileInfo{merged->file_info, expires_at};
        }
        outcome.status = Outcome::Status::Uploaded;
        outcome.file_info = merged->file_info;
        outcome.ttl_secs = merged->ttl_secs;
        return outcome;
    }
    outcome.error.kind = QqApiErrorKind::Unauthorized;
    outcome.error.detail = "unauthorized after token refresh";
    outcome.status = Outcome::Status::PermanentFail;
    return outcome;
}

}  // namespace lubancode::channel::qq
