// 更新助手 C++ 化·批二第②单:大文件断点续传下载器。语义唯一真源是
// scripts/updater.py 的 download_with_resume(约 L415-480)与护栏常量
// (L88-94),别单边发明:
//   - 重试口径:max_retries 是总尝试次数上限(python DOWNLOAD_RETRIES=3
//     即连首轮在内共试 3 次);失败间隔睡 backoff_base_secs^attempt 秒
//     (python DOWNLOAD_BACKOFF_BASE=2.0 → 2s/4s)。退避底数可注 0,CI
//     不真睡;
//   - 超时语义 = ConnectTimeout + LowSpeed 停滞探测(python 的
//     DOWNLOAD_TIMEOUT_SECS=60 是 urllib 每次读的空闲超时,对应 cpr 得用
//     LowSpeed"持续 N 秒平均速率低于限值即断",绝不拿 cpr::Timeout 当总
//     时长——慢链路上的大包会被拦腰砍断);
//   - 大小上限:有效帽 = min(声明的 size, hard_cap)(声明的必不超 4GiB
//     硬顶;python L91 MAX_ARCHIVE_BYTES);逐块验,超帽立即断流删 .part;
//   - Range 续传:dest 同目录下的 "<dest>.part" 存在则带
//     "Range: bytes=N-" 续传,206 续/200 重下从头(服务端不认 Range 时),
//     收尾一律整文件 Sha256Stream 对账,不符删 .part 报错;
//   - 重试只覆盖网络层错误(连接失败/停滞/半截流/5xx);4xx 不重试直接报
//     ——416 是"空续传边界"的例外:续传起点恰是声明大小时视为已收全,
//     照走整文件对账,对不上才删件报错。
// 进度:每收满 16MiB 调一次 on_progress(累计字节数,含续传前缀);成功
// 收尾追加一次终值回调(批三引擎层打 "[download] N MiB"收尾行用)。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::updater {

struct DownloadOptions {
    int max_retries = 3;             // 总尝试次数上限(照 python DOWNLOAD_RETRIES=3)
    double backoff_base_secs = 2.0;  // 退避底数,2^n 秒;测试可注 0(不真睡)
    std::uint64_t hard_cap_bytes = 4ull << 30;  // 硬顶,测试可注小值
    int connect_timeout_secs = 60;   // 连接阶段上限(秒)
    int low_speed_limit = 1;         // LowSpeed 停滞探测(字节/秒),停滞即断线重试
    int low_speed_window_secs = 30;  // 停滞判定窗口(秒)
};

// Range 断点续传下载:url 的体流式写到 "<dest>.part",逐块喂摘要、逐块验
// 帽,成功后整文件对账再原子改名成 dest。dest 的父目录不在会建。取消旗
// 置位即断,.part 保留已收字节作续传点。sha256_hex 须是 64 位十六进制
// (大小写均可,对账前折小写);declared_size 给了就参与封顶与完整性核对。
std::expected<void, std::string> DownloadWithResume(
    const std::string& url, const std::filesystem::path& dest, std::string_view sha256_hex,
    std::optional<std::uint64_t> declared_size, const DownloadOptions& options,
    const std::atomic<bool>* cancel, const std::function<void(std::uint64_t)>& on_progress);

}  // namespace lubancode::updater
