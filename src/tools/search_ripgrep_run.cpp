// BundledRipgrepRunner 的流式执行(ripgrep 迁移单 P0-4):ChildProcess 起
// rg、stdout/stderr 分流、grep 按 JSON Lines 分帧 / glob 按 NUL 分帧、四道墙
// (100 条 / 512 KiB 正文 / 16 KiB 单行 / 1 MiB 未完成帧)、满额主动收树、
// cancel/timeout/protocol error 各自终态,与退出码联合裁决(设计单 6.5~6.7)。
//
// 线程与生命周期(设计单 6.7,逐条对齐):
//   - 每次调用起一枚短命 rg,不做常驻 daemon;
//   - stdout/stderr 回调各在 ChildProcess 自己的读线程上跑;hits 与分帧
//     缓冲只有 stdout 线程写、stderr 缓冲只有 stderr 线程写,主线程在
//     Shutdown(join 读线程,给 happens-before)之后才读——无锁无险;
//   - JSON 解析只在 stdout 读线程推进,事件顺序天然保真;
//   - 主线程 WaitForExit 有界等(10ms 分片,不忙转),命中上限由 stdout
//     线程置本地原因旗;真正 Shutdown() 只由主线程这一个 owner 调,读线程
//     永不自 join;
//   - Run 返回前必先 Shutdown——任何异常路都不留 rg 进程(Windows Job
//     Object / POSIX 进程组收整棵树,rg 再生孩子也一并收)。

#include "tools/search_ripgrep.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/log_sink.hpp"        // LogSink:rg 兜底命中的来源一行
#include "platform/process.hpp"         // ChildProcess/WaitForExit:流式执行底座
#include "platform/text_encoding.hpp"   // SanitizeExternalText:bytes 路与 glob 路径的编码关口
#include "tools/path_utils.hpp"         // PathToUtf8

namespace lubancode::tools {

namespace {

// 本地原因旗(设计单 6.6):主动截断/取消/超时/协议错各一枚。终态裁决
// 先看它,退出码只兜"自然完成"那一条——主动停树后 rg 的非零退出不是失败。
enum class StopReason { None, LimitReached, Cancelled, Timeout, ProtocolError };

// 一次流式执行的会话状态。写者分工(无锁的根据,见文件头):
//   stdout 读线程:framer/hits/total_result_bytes/stats/protocol_note;
//   stderr 读线程:stderr_buf/stderr_overflow;
//   主线程:stop_reason(atomic,与 stdout 线程可能并发写,最后写者胜,
//           两边写后都立刻停,不会互相踩内容);
//   主线程读其余成员:只在 Shutdown(join 读线程)之后。
struct StreamSession {
    StreamSession(SearchMode mode_in, const RipgrepStreamLimits& lim)
        : mode(mode_in),
          framer(mode_in == SearchMode::Grep ? '\n' : '\0', lim.max_frame_bytes),
          limits(lim) {}

    // ---- stdout 读线程 ----------------------------------------------------

    bool OnStdoutChunk(std::string_view chunk) {
        if (stop_reason.load(std::memory_order_acquire) != StopReason::None) {
            return false;  // 主线程已判停(cancel/timeout),别再吃字节
        }
        const bool keep_reading =
            framer.Feed(chunk, [this](std::string_view frame) { return OnFrame(frame); });
        if (!keep_reading &&
            stop_reason.load(std::memory_order_acquire) == StopReason::None) {
            // Feed 返回 false 的两条路:帧回调判停(OnFrame 已置好自己的
            // 原因)或未完成帧超帽(这里补置协议错)——都有原因旗,主线程
            // 的有界等才会立刻醒来收树,不傻等到墙钟。
            stop_reason.store(StopReason::ProtocolError, std::memory_order_release);
        }
        return keep_reading;
    }

    // ---- stderr 读线程 ----------------------------------------------------

    void OnStderrChunk(std::string_view chunk) {
        const std::size_t room = limits.max_stderr_bytes - stderr_buf.size();
        if (room == 0) {
            stderr_overflow = chunk.empty() ? stderr_overflow : true;
            return;
        }
        if (chunk.size() > room) {
            stderr_buf.append(chunk.substr(0, room));
            stderr_overflow = true;
        } else {
            stderr_buf.append(chunk);
        }
    }

    // ---- 主线程(Shutdown 之后) ------------------------------------------

    // 自然 EOF 且无停因时补交尾帧:rg 的最后一行 JSON 可能没有换行
    //(设计单 6.5"最后一帧无换行也要处理")。有停因(满额/协议错/被杀)
    // 不冲刷——尾巴本来就不该再进结果。
    void FlushTailIfClean() {
        if (stop_reason.load(std::memory_order_acquire) != StopReason::None) {
            return;
        }
        (void)framer.FlushTail([this](std::string_view frame) { return OnFrame(frame); });
    }

    SearchMode mode;
    RipgrepStreamFramer framer;
    RipgrepStreamLimits limits;

    std::vector<SearchHit> hits;      // stdout 线程写;满 100 条(或 max_results)即停
    std::size_t total_result_bytes = 0;  // 第二道墙的账:path + 行正文合计
    std::optional<RipgrepStats> stats;   // summary 事件的诊断字段
    std::string protocol_note;           // 协议错的短诊断(坏 JSON 的前几十字节)

    std::string stderr_buf;         // stderr 线程写
    bool stderr_overflow = false;

    std::atomic<StopReason> stop_reason{StopReason::None};

private:
    bool OnFrame(std::string_view frame) {
        const bool ok = mode == SearchMode::Grep ? OnGrepFrame(frame) : OnGlobFrame(frame);
        if (!ok) {
            return false;
        }
        // 第二道墙:结果正文总量(glob 路径也计入)——与条数帽谁先到谁触发。
        if (total_result_bytes >= limits.max_total_result_bytes) {
            stop_reason.store(StopReason::LimitReached, std::memory_order_release);
            return false;
        }
        return true;
    }

    bool OnGrepFrame(std::string_view frame) {
        const ParsedGrepEvent event = ParseGrepEventLine(frame);
        if (event.kind == ParsedGrepEvent::Kind::Invalid) {
            // 坏 JSON 不吞掉装作无命中:记短诊断,按协议错收树。
            protocol_note.assign(frame.substr(0, 80));
            stop_reason.store(StopReason::ProtocolError, std::memory_order_release);
            return false;
        }
        if (event.kind == ParsedGrepEvent::Kind::Match) {
            SearchHit hit = std::move(event.hit);
            // 第三道墙:单条命中行截断(截,不是错)。
            hit.text = TruncateUtf8Boundary(hit.text, limits.max_hit_line_bytes);
            total_result_bytes += hit.path.size() + hit.text.size();
            hits.push_back(std::move(hit));
            // 第一道墙:命中行数满额即停——不看 rg 的退出码,主动收树。
            if (hits.size() >= limits.max_hits) {
                stop_reason.store(StopReason::LimitReached, std::memory_order_release);
                return false;
            }
        } else if (event.kind == ParsedGrepEvent::Kind::Summary) {
            stats = event.stats;  // 诊断字段,不改命中正文
        }
        return true;
    }

    bool OnGlobFrame(std::string_view frame) {
        if (frame.empty()) {
            return true;  // NUL 分帧不产空文件名;防御位
        }
        SearchHit hit;
        // glob 的帧就是路径字节:非法 UTF-8 文件名出门前清洗(设计单 10.3)。
        hit.path = NormalizeRipgrepPath(platform::SanitizeExternalText(std::string(frame)));
        total_result_bytes += hit.path.size();
        hits.push_back(std::move(hit));
        if (hits.size() >= limits.max_hits) {
            stop_reason.store(StopReason::LimitReached, std::memory_order_release);
            return false;
        }
        return true;
    }
};

// stderr 摘要给错误消息用:取第一行,限长,洗一遍——不把整条 argv、安装
// 路径或环境吐给模型(设计单 7.1)。
std::string FirstStderrLine(const std::string& stderr_text) {
    std::size_t end = stderr_text.find('\n');
    if (end == std::string::npos) {
        end = stderr_text.size();
    }
    std::string line = stderr_text.substr(0, std::min<std::size_t>(end, 200));
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return platform::SanitizeExternalText(line);
}

}  // namespace

// ---- 流式帽构造 -----------------------------------------------------------

RipgrepStreamLimits MakeRipgrepStreamLimits(std::size_t requested_max_results) {
    RipgrepStreamLimits limits;  // NSDMI 即合同值(四道墙 + 120s 墙钟)
    // max_results 是软请求:只能调低,不能调高——超过 100 条硬帽一律按帽截,
    // 缺省(0 视为缺省)走 100 条帽,与旧合同对齐(设计单 5.1 补段)。
    if (requested_max_results == 0) {
        requested_max_results = limits.max_hits;
    }
    limits.max_hits = std::min(requested_max_results, limits.max_hits);
    return limits;
}

// ---- 分帧器 ---------------------------------------------------------------

RipgrepStreamFramer::RipgrepStreamFramer(char delimiter, std::size_t max_frame_bytes)
    : delimiter_(delimiter), max_frame_bytes_(max_frame_bytes) {}

bool RipgrepStreamFramer::Feed(std::string_view chunk, const FrameCallback& on_frame) {
    std::size_t begin = 0;
    while (true) {
        const std::size_t delim = chunk.find(delimiter_, begin);
        if (delim == std::string_view::npos) {
            break;
        }
        pending_.append(chunk.substr(begin, delim - begin));
        const bool keep = DeliverPending(on_frame);
        if (!keep) {
            return false;  // 帧回调判停(满额/总帽/协议错)
        }
        begin = delim + 1;
    }
    pending_.append(chunk.substr(begin));
    if (pending_.size() > max_frame_bytes_) {
        return false;  // 第四道墙:未完成帧超帽(协议错),停读收树
    }
    return true;
}

bool RipgrepStreamFramer::FlushTail(const FrameCallback& on_frame) {
    if (pending_.empty()) {
        return true;
    }
    return DeliverPending(on_frame);
}

bool RipgrepStreamFramer::DeliverPending(const FrameCallback& on_frame) {
    const bool keep = on_frame(std::string_view(pending_));
    pending_.clear();
    return keep;
}

// ---- base64 与文本两路 -----------------------------------------------------

std::string DecodeBase64(std::string_view text) {
    // 标准字母表 + '=' 填充;遇到空白跳过(rg 不产,防御),遇到别的字符
    // 即弃(返回空串,调用方按"解码不出"处理,不塞半截乱码)。
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    int group[4];
    int n = 0;
    bool done = false;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            done = true;  // 填充后不该再有数据
            continue;
        }
        if (done) {
            return std::string();  // 填充后又冒字符:不是合法 base64
        }
        const int v = value_of(c);
        if (v < 0) {
            return std::string();
        }
        group[n++] = v;
        if (n == 4) {
            out += static_cast<char>((group[0] << 2) | (group[1] >> 4));
            out += static_cast<char>(((group[1] & 0xF) << 4) | (group[2] >> 2));
            out += static_cast<char>(((group[2] & 0x3) << 6) | group[3]);
            n = 0;
        }
    }
    if (n == 1) {
        return std::string();  // 落单字符凑不成组
    }
    if (n == 2) {
        out += static_cast<char>((group[0] << 2) | (group[1] >> 4));
    } else if (n == 3) {
        out += static_cast<char>((group[0] << 2) | (group[1] >> 4));
        out += static_cast<char>(((group[1] & 0xF) << 4) | (group[2] >> 2));
    }
    return out;
}

namespace {

// rg --json 的 path/lines 两路:{"text": ...} 直用;{"bytes": "<base64>"}
// 解码后过 SanitizeExternalText——非 UTF-8 输入不允许把非法字节直接塞进
// JSON/history(设计单 6.5)。两路都没有返回 nullopt(事件形状不合)。
std::optional<std::string> DecodeTextOrBytes(const nlohmann::json& node) {
    if (!node.is_object()) {
        return std::nullopt;
    }
    const auto text = node.find("text");
    if (text != node.end() && text->is_string()) {
        return text->get<std::string>();
    }
    const auto bytes = node.find("bytes");
    if (bytes != node.end() && bytes->is_string()) {
        const std::string decoded = DecodeBase64(bytes->get<std::string>());
        if (decoded.empty() && !bytes->get<std::string>().empty()) {
            // base64 解不出:按空处理,不把乱码往结果里塞。
            return std::string();
        }
        return platform::SanitizeExternalText(decoded);
    }
    return std::nullopt;
}

}  // namespace

std::string NormalizeRipgrepPath(std::string_view path_utf8) {
    std::string out(path_utf8);
    std::replace(out.begin(), out.end(), '\\', '/');
    // 目录 root 下 scope=".",rg 输出天然带 "./" 前缀;单文件 root 无前缀。
    // 只剥一次:路径本体以 "./" 开头(比如真有个 ./x 目录)在 scope="."
    // 的口径下不会出现,防御位留着不叠剥。
    if (out.rfind("./", 0) == 0) {
        out.erase(0, 2);
    }
    return out;
}

std::string TruncateUtf8Boundary(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t cut = max_bytes;
    // 往回收敛,不切半个多字节字符: continuation 字节是 10xxxxxx。
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.substr(0, cut));
}

ParsedGrepEvent ParseGrepEventLine(std::string_view line) {
    ParsedGrepEvent out;
    // allow_exceptions=false:坏 JSON 得 discarded,不抛——回调线程里不养异常。
    const nlohmann::json event =
        nlohmann::json::parse(line.begin(), line.end(), /*callback=*/nullptr,
                              /*allow_exceptions=*/false, /*ignore_comments=*/false);
    if (event.is_discarded() || !event.is_object()) {
        out.kind = ParsedGrepEvent::Kind::Invalid;
        return out;
    }
    const auto type = event.find("type");
    if (type == event.end() || !type->is_string()) {
        out.kind = ParsedGrepEvent::Kind::Invalid;
        return out;
    }
    const std::string kind = type->get<std::string>();
    if (kind == "match") {
        const auto data = event.find("data");
        if (data == event.end() || !data->is_object()) {
            out.kind = ParsedGrepEvent::Kind::Invalid;
            return out;
        }
        const auto path = data->find("path");
        const auto lines = data->find("lines");
        const auto line_number = data->find("line_number");
        if (path == data->end() || lines == data->end()) {
            out.kind = ParsedGrepEvent::Kind::Invalid;
            return out;
        }
        const std::optional<std::string> path_text = DecodeTextOrBytes(*path);
        const std::optional<std::string> line_text = DecodeTextOrBytes(*lines);
        if (!path_text.has_value() || !line_text.has_value()) {
            out.kind = ParsedGrepEvent::Kind::Invalid;
            return out;
        }
        out.kind = ParsedGrepEvent::Kind::Match;
        out.hit.path = NormalizeRipgrepPath(*path_text);
        out.hit.text = *line_text;
        // rg 的 lines.text 带尾换行(CRLF 文件带 "\r\n"):按行合同剥掉。
        if (!out.hit.text.empty() && out.hit.text.back() == '\n') {
            out.hit.text.pop_back();
            if (!out.hit.text.empty() && out.hit.text.back() == '\r') {
                out.hit.text.pop_back();
            }
        }
        if (line_number != data->end() && line_number->is_number_integer()) {
            out.hit.line_number = line_number->get<long long>();
        }
        return out;
    }
    if (kind == "summary") {
        out.kind = ParsedGrepEvent::Kind::Summary;
        const auto data = event.find("data");
        if (data != event.end() && data->is_object()) {
            const auto stats = data->find("stats");
            if (stats != data->end() && stats->is_object()) {
                const auto number = [&stats](const char* field) -> std::uint64_t {
                    const auto node = stats->find(field);
                    return node != stats->end() && node->is_number_unsigned()
                               ? node->get<std::uint64_t>()
                               : 0;
                };
                out.stats.searches = number("searches");
                out.stats.searches_with_match = number("searches_with_match");
                out.stats.matched_lines = number("matched_lines");
            }
            const auto elapsed = data->find("elapsed_total");
            if (elapsed != data->end() && elapsed->is_object()) {
                const auto secs = elapsed->find("secs");
                const auto nanos = elapsed->find("nanos");
                double total = 0.0;
                if (secs != elapsed->end() && secs->is_number()) {
                    total += secs->get<double>();
                }
                if (nanos != elapsed->end() && nanos->is_number()) {
                    total += nanos->get<double>() / 1e9;
                }
                out.stats.elapsed_total_secs = total;
            }
        }
        return out;
    }
    // begin/end/context/desert 等其余稳定事件:忽略(设计单 6.5)。
    out.kind = ParsedGrepEvent::Kind::Other;
    return out;
}

// ---- 生产 runner -----------------------------------------------------------

BundledRipgrepRunner::BundledRipgrepRunner(std::filesystem::path exe_override,
                                           RipgrepVersionProbe version_probe,
                                           RipgrepStreamLimits limits)
    : version_probe_(std::move(version_probe)), limits_(std::move(limits)) {
    // 缺省实参 {} 是"空路径",不是"注入了空路径":空一律按"没注入"算,
    // 走生产三层发现。不然默认构造会拿空路径去过文件校验,永远报缺件——
    // P0-2 起这枚暗雷就埋在缺省实参里,默认构造的测试只断言"缺件也算诚实
    // 终态"没炸出来,P0-5 切主路才现形。
    if (!exe_override.empty()) {
        exe_override_ = std::move(exe_override);
    }
}

BundledRipgrepRunner::BundledRipgrepRunner(const Overrides& overrides)
    : version_probe_(std::move(overrides.version_probe)), limits_(std::move(overrides.limits)) {
    if (overrides.exe.has_value() && !overrides.exe->empty()) {
        exe_override_ = overrides.exe;
    } else if (!overrides.candidates.empty()) {
        candidates_override_ = std::move(overrides.candidates);
    }
}

RipgrepSmokeResult BundledRipgrepRunner::smoke_result() const {
    std::lock_guard<std::mutex> lock(smoke_mutex_);
    return smoke_result_;
}

void BundledRipgrepRunner::EnsureSmoke() {
    std::lock_guard<std::mutex> lock(smoke_mutex_);
    if (smoke_done_) {
        return;  // 每实例只 smoke 一次(工具实例共享,别让并发调用各起一遍进程)
    }
    // 定位:单枚注入(测试,不回退)优先;否则整批候选(注入或生产)走
    // 三层发现——随包 → rg-stage → PATH,命中件照过版本门。
    if (exe_override_.has_value()) {
        smoke_result_ = RunRipgrepSmoke(*exe_override_, version_probe_);
        smoke_done_ = true;
        return;
    }
    const std::vector<RipgrepCandidate> candidates =
        candidates_override_.has_value() ? *candidates_override_ : CollectRipgrepCandidates();
    const RipgrepDiscovery discovery = DiscoverRipgrep(candidates);
    if (!discovery.hit.has_value()) {
        // 三层全缺:稳定错照旧,文案升级成修复指引(不裸抛路径)。
        smoke_result_.status = RipgrepSmokeStatus::Missing;
        smoke_result_.code = SearchBackendError::BackendMissing;
        smoke_result_.message = FormatRipgrepAllMissingGuidance(discovery.tiers);
        smoke_result_.exe = discovery.tiers.empty() ? std::filesystem::path{}
                                                    : discovery.tiers.front().candidate.exe;
        smoke_done_ = true;
        return;
    }
    smoke_result_ = RunRipgrepSmoke(discovery.hit->exe, version_probe_);
    smoke_result_.source = discovery.hit->source;
    if (discovery.hit->source != RipgrepSource::Bundled) {
        // 自愈一行账(单子 §四层 2):exe 旁缺件,兜底层有货,直接用兜底
        // 路径不拷贝。落 LogSink(默认 stderr,装配了文件 sink 落日志),
        // 不进工具正文——正文只留搜索结果。
        platform::LogSink::Instance().Warn(
            "search", std::string("rg 兜底命中 ") + std::string(ToString(discovery.hit->source)) +
                          " 层: " + PathToUtf8(discovery.hit->exe) + "(exe 旁 libexec 缺件;直接用该路径,不拷贝)");
    }
    smoke_done_ = true;
}

std::expected<RipgrepRunResult, SearchBackendErrorInfo>
BundledRipgrepRunner::Run(const SearchRequest& request, const SearchPolicy& policy,
                          const ToolExecutionContext& context) {
    // 起跑前取消(设计单 10.5):不起进程,直接按取消收口。
    if (context.cancel != nullptr && context.cancel->load(std::memory_order_relaxed)) {
        return std::unexpected(SearchBackendErrorInfo{SearchBackendError::Cancelled, "搜索已取消"});
    }

    // 前置:定位 -> 缺件/不可执行/版本 smoke(每实例一次,四路稳定错误)。
    EnsureSmoke();
    std::filesystem::path rg_exe;
    {
        std::lock_guard<std::mutex> lock(smoke_mutex_);
        if (smoke_result_.status != RipgrepSmokeStatus::Ready) {
            return std::unexpected(SearchBackendErrorInfo{
                smoke_result_.code.value_or(SearchBackendError::BackendMissing),
                smoke_result_.message});
        }
        rg_exe = smoke_result_.exe;
    }

    // 流式帽:runner 的墙(默认即合同值,测试可注入小帽)叠 max_results 软请求
    //(只降不升,见 MakeRipgrepStreamLimits)。
    RipgrepStreamLimits limits = limits_;
    limits.max_hits =
        std::min(MakeRipgrepStreamLimits(request.max_results).max_hits, limits_.max_hits);

    // argv 纯函数(设计单 6.3/6.4):绝对路径直起进程,不经 shell、不搜 PATH。
    const RipgrepInvocation invocation =
        request.mode == SearchMode::Grep ? BuildGrepArgv(request, policy, rg_exe)
                                         : BuildGlobArgv(request, policy, rg_exe);

    StreamSession session(request.mode, limits);
    platform::ChildProcess process;

    const platform::SpawnResult spawn = process.Start(
        PathToUtf8(rg_exe), invocation.args, /*env=*/{},
        [&session](std::string_view chunk) { return session.OnStdoutChunk(chunk); },
        [&session](std::string_view chunk) { session.OnStderrChunk(chunk); },
        invocation.cwd_utf8);
    if (!spawn.success) {
        return std::unexpected(
            SearchBackendErrorInfo{SearchBackendError::SpawnFailed, "起 ripgrep 失败: " + spawn.error});
    }

    // 主线程有界等(设计单 6.7):WaitForExit 10ms 分片,不忙转;cancel、
    // stdout 线程的停因、墙钟三者谁先到谁触发。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeout_ms);
    while (true) {
        if (context.cancel != nullptr && context.cancel->load(std::memory_order_relaxed)) {
            session.stop_reason.store(StopReason::Cancelled, std::memory_order_release);
            break;
        }
        if (session.stop_reason.load(std::memory_order_acquire) != StopReason::None) {
            break;  // stdout 线程判停:满额/总帽/协议错
        }
        const int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  std::chrono::steady_clock::now())
                .count());
        if (remaining_ms <= 0) {
            session.stop_reason.store(StopReason::Timeout, std::memory_order_release);
            break;
        }
        process.WaitForExit(std::min(remaining_ms, 25), context.cancel);
        if (!process.IsAlive()) {
            break;  // 自然退出,等读线程把尾巴读到 EOF(Shutdown 里 join)
        }
    }

    // 收树(唯一 owner 调):先关 stdin 给体面退出留 200ms,再 Job/进程组
    // 硬杀,末了 join 读线程——此后 hits/stderr/exit_code 才可安全读。
    process.Shutdown(200);
    session.FlushTailIfClean();

    // ---- 终态裁决(设计单 6.6):本地原因旗优先,退出码只兜自然完成 ------
    const StopReason stop = session.stop_reason.load(std::memory_order_acquire);
    if (stop == StopReason::Cancelled) {
        return std::unexpected(SearchBackendErrorInfo{SearchBackendError::Cancelled, "搜索已取消"});
    }
    if (stop == StopReason::Timeout) {
        return std::unexpected(SearchBackendErrorInfo{
            SearchBackendError::Timeout,
            "搜索超时(超过 " + std::to_string(limits.timeout_ms / 1000) +
                " 秒收树;可缩小 path 范围或把 pattern 写得更具体)"});
    }
    if (stop == StopReason::ProtocolError) {
        return std::unexpected(SearchBackendErrorInfo{
            SearchBackendError::ProtocolError,
            "ripgrep 输出协议坏了(坏 JSON 或未完成帧超限): " +
                platform::SanitizeExternalText(session.protocol_note)});
    }

    RipgrepRunResult result;
    result.hits = std::move(session.hits);
    result.exit_code = process.exit_code();
    result.stats = session.stats;
    result.stderr_text = session.stderr_buf;
    if (session.stderr_overflow) {
        result.stderr_text += "\n…(stderr 超出 " + std::to_string(limits.max_stderr_bytes / 1024) +
                              " KiB 已截断)";
    }

    if (stop == StopReason::LimitReached) {
        // 满额/总帽主动收树:success + truncated,rg 的非零退出不作数,
        // 绝不借退出码冒充失败(设计单 6.6 第 4 行)。
        result.truncated = true;
        return result;
    }

    // 自然完成:按退出码。0=有命中;1=无命中(都是 success);
    // 2=pattern/glob 不合法或 IO 硬错;信号(POSIX 记负数)/其他码=进程半途死,
    // 不冒充"没搜到"。
    const int exit_code = result.exit_code;
    if (exit_code == 0 || exit_code == 1) {
        return result;
    }
    if (exit_code == 2) {
        // pattern 编译错在打印任何 stdout 之前就退出——零命中 + 退出码 2 =
        // 模型输入的 pattern/glob 不合法,稳定错误 + 洗过的 stderr 首句。
        // 已有命中还退出 2 的是个别文件 IO 噪声(权限等):结果保住,stderr
        // 随账带回,不因一棵树里一个坏文件丢掉整场搜索。
        if (result.hits.empty()) {
            return std::unexpected(SearchBackendErrorInfo{
                SearchBackendError::PatternInvalid,
                "pattern 不合法: " + FirstStderrLine(result.stderr_text)});
        }
        return result;
    }
    return std::unexpected(SearchBackendErrorInfo{
        SearchBackendError::RunFailed,
        "ripgrep 异常退出(码 " + std::to_string(exit_code) + ")" +
            (result.stderr_text.empty() ? std::string() : ": " + FirstStderrLine(result.stderr_text))});
}

}  // namespace lubancode::tools
