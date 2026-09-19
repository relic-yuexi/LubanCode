// DownloadWithResume 的实现:断点续传下载器(批二第②单)。语义对齐与
// 行为契约见 download.hpp 文件头;python 侧真源是 scripts/updater.py 的
// download_with_resume(L415-480)。差异两处都有明确理由,批三引擎层对接
// 时留意:
//   - 4xx 不重试(python 的 HTTPError 是 URLError 子类,4xx/5xx 一锅端进
//     重试;确定性错误重试是白搭)——416 例外,见 hpp;
//   - Content-Length/Content-Range 与声明大小对不上时头阶段就断流删
//     .part(python 没这道验,靠收尾摘要兜);续传基底既然对不上号,
//     留着只会把下一轮带沟里。

#include "updater/download.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <cpr/cpr.h>

#include "platform/network_proxy.hpp"
#include "platform/paths.hpp"  // PathToUtf8/ReplaceFileAtomically:.part → dest 跨平台盖写
#include "platform/sha256.hpp"

namespace lubancode::updater {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kProgressMarkBytes = 16ull << 20;  // 进度节拍:每满 16 MiB 一报
constexpr std::size_t kHashChunkBytes = 256ull << 10;      // 对账读盘块:256 KiB,绝不整读进内存

std::string LowerAscii(std::string_view value) {
    std::string out(value);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

// 状态行("HTTP/1.1 206 Partial Content")里抠码,抠不出给 0。
int StatusCodeFromLine(std::string_view line) {
    if (line.rfind("HTTP/", 0) != 0) return 0;
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) return 0;
    int code = 0;
    const char* first = line.data() + space + 1;
    const char* last = line.data() + line.size();
    if (std::from_chars(first, last, code).ec != std::errc{}) return 0;
    return code;
}

std::optional<std::uint64_t> ParseU64(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

// "bytes 8-15/16" → {first=8, total=16};"bytes 8-15/*" 的 total 是 nullopt。
// 不认的形状整体 nullopt——此时不较真,交给收尾整文件对账兜底(python 就
// 不验 Content-Range,这里只在能解析时加严)。
struct ContentRange {
    std::uint64_t first = 0;
    std::optional<std::uint64_t> total;
};

std::optional<ContentRange> ParseContentRange(std::string_view value) {
    const std::string lowered = LowerAscii(value);
    const std::size_t unit = lowered.find("bytes");
    if (unit == std::string::npos) return std::nullopt;
    std::size_t cursor = unit + 5;
    while (cursor < lowered.size() && lowered[cursor] == ' ') cursor++;
    const std::size_t dash = lowered.find('-', cursor);
    if (dash == std::string::npos) return std::nullopt;
    const auto first = ParseU64(std::string_view(lowered).substr(cursor, dash - cursor));
    if (!first.has_value()) return std::nullopt;
    ContentRange range;
    range.first = *first;
    const std::size_t slash = lowered.find('/', dash);
    if (slash == std::string::npos) return std::nullopt;
    const std::string_view tail = std::string_view(lowered).substr(slash + 1);
    if (tail == "*") return range;
    range.total = ParseU64(tail);
    if (!range.total.has_value()) return std::nullopt;
    return range;
}

// 错误响应体的短摘:压成一行、截到 limit,只给人看的报错文案用。
std::string SingleLineExcerpt(std::string_view body, std::size_t limit) {
    std::string out;
    out.reserve(std::min(body.size(), limit));
    for (const char ch : body) {
        if (out.size() >= limit) break;
        out.push_back(ch == '\r' || ch == '\n' ? ' ' : ch);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// 整文件流式对账:一块一块喂 Sha256Stream,绝不整读(4GiB 硬顶的整包也
// 只有常量内存)。
std::expected<std::string, std::string> HashFileHex(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::unexpected("对账读盘失败: " + platform::PathToUtf8(path));
    platform::Sha256Stream digest;
    std::vector<char> buffer(kHashChunkBytes);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) digest.Update(std::string_view(buffer.data(), static_cast<std::size_t>(got)));
    }
    if (file.bad()) return std::unexpected("对账读盘失败: " + platform::PathToUtf8(path));
    return digest.FinalHex();
}

// 退避睡法:切成 20ms 的小片轮询取消旗,大退避里取消也不至于干等。
void SleepCancelAware(double seconds, const std::atomic<bool>* cancel) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(seconds));
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel != nullptr && cancel->load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

enum class AttemptOutcome {
    kSuccess,    // 流走完且状态合法(或 416 已收全边界),.part 是完整候选
    kRetryable,  // 网络层错(连接/停滞/半截/写盘/5xx),.part 保留作续传点
    kFatal,      // 语义错(4xx/超帽/大小对不上),直接报
    kCancelled,  // 取消旗
};

struct AttemptResult {
    AttemptOutcome outcome = AttemptOutcome::kRetryable;
    std::string message;          // 失败原因的人话
    std::uint64_t written = 0;    // kSuccess 时:收尾累计字节(含续传前缀)
    bool delete_partial = false;  // 收场是否连 .part 删净(超帽/坏基底类)
};

// 一轮尝试:发一次 GET(带不带 Range 由 offset 定),流式收体落 .part。
// 只管一轮的成败分型,不睡退避不重试——那是外层循环的事。
AttemptResult AttemptDownload(const std::string& url, const fs::path& partial, std::uint64_t offset,
                              std::optional<std::uint64_t> declared_size, std::uint64_t cap,
                              const DownloadOptions& options, const std::atomic<bool>* cancel,
                              const std::function<void(std::uint64_t)>& on_progress) {
    AttemptResult result;

    int status_code = 0;
    std::optional<std::uint64_t> content_length;
    std::optional<ContentRange> content_range;
    bool cancelled = false;
    bool cap_exceeded = false;
    bool semantics_checked = false;
    // 头/写阶段攒下的两类断流原因:语义错(确定性,不重试)与盘错(按
    // python 的 OSError 口径归网络层可重试)。
    std::string header_fatal;
    std::string disk_error;

    std::uint64_t written = offset;
    std::uint64_t next_mark = (offset / kProgressMarkBytes + 1) * kProgressMarkBytes;
    std::ofstream out;
    bool out_opened = false;
    std::string error_body;

    const auto header_cb = [&](const std::string_view& header, intptr_t) -> bool {
        const int code = StatusCodeFromLine(header);
        if (code != 0) {
            status_code = code;
            return true;
        }
        const std::size_t colon = header.find(':');
        if (colon == std::string_view::npos || colon == 0) return true;
        const std::string name = LowerAscii(header.substr(0, colon));
        std::string_view value = header.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
            value.remove_suffix(1);
        }
        if (name == "content-length") {
            content_length = ParseU64(value);
        } else if (name == "content-range") {
            content_range = ParseContentRange(value);
        }
        return true;
    };

    const auto write_cb = [&](const std::string_view& data, intptr_t) -> bool {
        if (cancel != nullptr && cancel->load()) {
            cancelled = true;
            return false;
        }
        if (cap_exceeded || !header_fatal.empty() || !disk_error.empty()) return false;
        // 体首字节到达,头已收齐:先验语义,再开文件。
        if (!semantics_checked) {
            semantics_checked = true;
            if (status_code == 200 && declared_size.has_value() && content_length.has_value() &&
                *content_length != *declared_size) {
                header_fatal = "声明大小对不上:清单 " + std::to_string(*declared_size) + " 字节,服务端 Content-Length " +
                               std::to_string(*content_length) + " 字节";
            } else if (status_code == 206 && content_range.has_value()) {
                if (content_range->first != offset) {
                    header_fatal = "206 续段错位:请求 bytes=" + std::to_string(offset) + "-,服务端从 " +
                                   std::to_string(content_range->first) + " 起";
                } else if (declared_size.has_value() && content_range->total.has_value() &&
                           *content_range->total != *declared_size) {
                    header_fatal = "声明大小对不上:清单 " + std::to_string(*declared_size) + " 字节,Content-Range 总长 " +
                                   std::to_string(*content_range->total) + " 字节";
                }
            }
            if (status_code == 206 && declared_size.has_value() && content_length.has_value() &&
                header_fatal.empty()) {
                const std::uint64_t expected_rest = *declared_size - offset;
                if (*content_length != expected_rest) {
                    header_fatal = "声明大小对不上:应余 " + std::to_string(expected_rest) +
                                   " 字节,服务端 Content-Length " + std::to_string(*content_length) + " 字节";
                }
            }
            if (!header_fatal.empty()) return false;
        }
        const bool body_ok = status_code == 200 || status_code == 206;
        if (!body_ok) {
            // 非 200/206 的响应体不进 .part,攒起来塞报错文案(python L445:
            // 除这两态一律报,体只留给人看)。
            if (error_body.size() < 4096) error_body.append(data);
            return true;
        }
        if (!out_opened) {
            // 200 = 服务端不认 Range 从头来(旧 .part 作废,truncate);206 =
            // 接 .part 尾巴续。进度标记随新起点重算。
            if (status_code == 200) {
                written = 0;
                next_mark = kProgressMarkBytes;
                out.open(partial, std::ios::binary | std::ios::trunc);
            } else {
                out.open(partial, std::ios::binary | std::ios::app);
            }
            if (!out.is_open()) {
                disk_error = ".part 打不开: " + platform::PathToUtf8(partial);
                return false;
            }
            out_opened = true;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out.good()) {
            disk_error = "写盘失败: " + platform::PathToUtf8(partial);
            return false;
        }
        written += data.size();
        if (written > cap) {
            cap_exceeded = true;
            return false;
        }
        if (written >= next_mark) {
            if (on_progress) on_progress(written);
            next_mark += kProgressMarkBytes;
        }
        return true;
    };

    // 连接/握手阶段没有响应体,WriteCallback 不触发——取消只挂在这里才能
    // 掐得动"连不上/装死"的服务端(同 api::PostSseStream 的挂法)。
    const auto progress_cb = [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                 cpr::cpr_pf_arg_t, intptr_t) -> bool {
        if (cancel != nullptr && cancel->load()) {
            cancelled = true;
            return false;
        }
        return true;
    };

    cpr::Header headers{{"User-Agent", "lubancode-updater"}};
    if (offset > 0) headers["Range"] = "bytes=" + std::to_string(offset) + "-";

    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetHeader(headers);
    session.SetConnectTimeout(cpr::ConnectTimeout{std::chrono::seconds(options.connect_timeout_secs)});
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // 新版 cpr 弃用 int 构造改 chrono;vendored 1.11 只有 int 形,值两边通用
#endif
    // 超时语义 = 连接上限 + LowSpeed 停滞探测,绝不设 cpr::Timeout——那是
    // 总时长墙,慢链路上的大包会被拦腰砍断(python 的 60s 是每次读的空闲
    // 超时,对应的只有 LowSpeed 这一副)。
    session.SetLowSpeed(cpr::LowSpeed{options.low_speed_limit, options.low_speed_window_secs});
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    session.SetHeaderCallback(cpr::HeaderCallback(header_cb));
    session.SetWriteCallback(cpr::WriteCallback(write_cb));
    session.SetProgressCallback(cpr::ProgressCallback(progress_cb));
    // 代理走系统判定(环境变量留给 libcurl,Windows 注册表手动代理在这取);
    // 只挂 https 栏,http 直连目标(如测试的 127.0.0.1)不受牵连。
    if (const auto proxy = platform::SystemProxyForScheme("https"); proxy.has_value()) {
        session.SetProxies(cpr::Proxies{{"https", *proxy}});
    }
    const cpr::Response response = session.Get();

    // 收场分型,顺序有讲究:取消 > 超帽 > 语义错 > 盘错 > 网络错 > 状态码。
    if (cancelled || (cancel != nullptr && cancel->load())) {
        result.outcome = AttemptOutcome::kCancelled;
        result.message = "下载被取消信号中止";
        return result;
    }
    if (cap_exceeded) {
        result.outcome = AttemptOutcome::kFatal;
        result.delete_partial = true;
        result.message = "下载超过大小上限(" + std::to_string(cap) + " 字节,声明的 " +
                         (declared_size.has_value() ? std::to_string(*declared_size) : std::string("硬顶")) + ")";
        return result;
    }
    if (!header_fatal.empty()) {
        result.outcome = AttemptOutcome::kFatal;
        result.delete_partial = true;  // 续传基底对不上号,留着必带偏下一轮
        result.message = header_fatal;
        return result;
    }
    if (!disk_error.empty()) {
        result.outcome = AttemptOutcome::kRetryable;  // python 的 OSError 也在重试之列
        result.message = disk_error;
        return result;
    }
    if (response.error) {
        result.outcome = AttemptOutcome::kRetryable;
        result.message = "网络错误(curl " + std::to_string(static_cast<int>(response.error.code)) +
                         "): " + response.error.message;
        return result;
    }
    if (status_code == 416) {
        // 空续传边界:起点恰是声明大小,.part 疑似已收全——照走整文件对账,
        // 摘要对得上就当成功,对不上由外层删件报错。
        if (offset > 0 && declared_size.has_value() && *declared_size == offset) {
            result.outcome = AttemptOutcome::kSuccess;
            result.written = offset;
            return result;
        }
        result.outcome = AttemptOutcome::kFatal;
        result.delete_partial = true;  // 服务端文件比 .part 短,基底必坏
        result.message = "下载返回 HTTP 416(续传起点 " + std::to_string(offset) + " 字节,服务端文件不够长)";
        return result;
    }
    if (status_code != 200 && status_code != 206) {
        const std::string excerpt = SingleLineExcerpt(error_body, 160);
        if (status_code >= 500 && status_code <= 599) {
            result.outcome = AttemptOutcome::kRetryable;
            result.message = "HTTP " + std::to_string(status_code) + (excerpt.empty() ? "" : ": " + excerpt);
            return result;
        }
        result.outcome = AttemptOutcome::kFatal;  // 4xx 不重试:确定性错误
        result.message = "下载返回 HTTP " + std::to_string(status_code) + (excerpt.empty() ? "" : ": " + excerpt);
        return result;
    }
    // 200 但零体:文件没开过,显式 truncate 一把,把旧 .part 清干净(python
    // 的 mode="wb" 在 open 时就干了这件事)。
    if (!out_opened && status_code == 200) {
        out.open(partial, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            result.outcome = AttemptOutcome::kRetryable;
            result.message = ".part 打不开: " + platform::PathToUtf8(partial);
            return result;
        }
        out_opened = true;
        written = 0;
    }
    if (out_opened) {
        out.flush();
        out.close();
        if (out.fail()) {
            result.outcome = AttemptOutcome::kRetryable;
            result.message = "写盘失败: " + platform::PathToUtf8(partial);
            return result;
        }
    }
    result.outcome = AttemptOutcome::kSuccess;
    result.written = written;
    return result;
}

}  // namespace

std::expected<void, std::string> DownloadWithResume(
    const std::string& url, const fs::path& dest, std::string_view sha256_hex,
    std::optional<std::uint64_t> declared_size, const DownloadOptions& options,
    const std::atomic<bool>* cancel, const std::function<void(std::uint64_t)>& on_progress) {
    fs::path partial = dest;
    partial += ".part";

    if (!dest.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        if (ec) {
            return std::unexpected("建目录失败: " + platform::PathToUtf8(dest.parent_path()) + ": " + ec.message());
        }
    }

    // 有效帽 = min(声明大小, 硬顶):声明的必不超 4GiB 硬顶,测试注小帽
    // 也从这进(python L421 的 cap 只取声明侧,C++ 侧把硬顶立成绝对墙)。
    const std::uint64_t cap = declared_size.has_value()
                                  ? (std::min)(*declared_size, options.hard_cap_bytes)
                                  : options.hard_cap_bytes;
    const std::string wanted_hex = LowerAscii(sha256_hex);

    const int attempts = options.max_retries > 0 ? options.max_retries : 1;
    std::string last_error;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected("下载被取消信号中止");
        }
        std::uint64_t offset = 0;
        std::error_code size_ec;
        if (fs::is_regular_file(partial, size_ec)) {
            const auto size = fs::file_size(partial, size_ec);
            if (!size_ec) {
                offset = size;
                if (offset > cap) offset = 0;  // 旧 .part 超帽:作废从头来(python L430-432)
            }
        }

        const AttemptResult attempt_result =
            AttemptDownload(url, partial, offset, declared_size, cap, options, cancel, on_progress);
        switch (attempt_result.outcome) {
        case AttemptOutcome::kCancelled:
            return std::unexpected("下载被取消信号中止");
        case AttemptOutcome::kFatal: {
            std::error_code remove_ec;
            if (attempt_result.delete_partial) fs::remove(partial, remove_ec);
            return std::unexpected(attempt_result.message);
        }
        case AttemptOutcome::kSuccess: {
            // 完整性:收到的字节数须与声明合(416 边界的"已收全"也走这里;
            // python L462-464)。
            if (declared_size.has_value() && attempt_result.written != *declared_size) {
                return std::unexpected("下载不完整:得了 " + std::to_string(attempt_result.written) +
                                       " 字节,声明 " + std::to_string(*declared_size));
            }
            const auto actual = HashFileHex(partial);
            if (!actual.has_value()) return std::unexpected(actual.error());
            if (*actual != wanted_hex) {
                std::error_code remove_ec;
                fs::remove(partial, remove_ec);
                return std::unexpected("摘要不符:期望 sha256:" + std::string(sha256_hex) +
                                       ",实得 sha256:" + *actual);
            }
            const auto renamed = platform::ReplaceFileAtomically(partial, dest);
            if (!renamed.has_value()) return std::unexpected(renamed.error());
            if (on_progress) on_progress(attempt_result.written);  // 终值一次,收尾行用
            return {};
        }
        case AttemptOutcome::kRetryable:
        default:
            last_error = attempt_result.message;
            break;
        }
        if (attempt < attempts && options.backoff_base_secs > 0.0) {
            // 退避 base^attempt(python L477:2.0 → 2s/4s);切片睡,取消不等满。
            SleepCancelAware(std::pow(options.backoff_base_secs, static_cast<double>(attempt)), cancel);
        }
    }
    return std::unexpected("下载失败(重试 " + std::to_string(attempts) + " 次): " + last_error);
}

}  // namespace lubancode::updater
