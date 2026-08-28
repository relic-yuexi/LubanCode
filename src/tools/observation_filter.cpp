#include "tools/observation_filter.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "tools/path_utils.hpp"  // PathToUtf8:提示文案里的路径出 UTF-8

namespace lubancode::tools {

ObservationBoundary& ObservationBoundary::Instance() {
    static ObservationBoundary instance;
    return instance;
}

namespace {

// 规范化到绝对路径:weakly_canonical 不要求路径已存在(已存在的前段会
// 解析成盘上的真实拼写——大小写、符号链接),再失败退 absolute。两侧
// (登记与查询)都过这一道,才比得齐。
std::filesystem::path NormalizeAbsolute(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    std::error_code abs_ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, abs_ec);
    if (!abs_ec) {
        return absolute.lexically_normal();
    }
    return path.lexically_normal();
}

// 一段路径相等不:Windows 盘不区分大小写,按小写比;POSIX 按原文。
bool SamePathComponent(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    std::string l = PathToUtf8(left);
    std::string r = PathToUtf8(right);
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return l == r;
#else
    return left == right;
#endif
}

}  // namespace

void ObservationBoundary::AddExcludedDir(const std::filesystem::path& dir) {
    std::filesystem::path normalized = NormalizeAbsolute(dir);
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(dirs_.begin(), dirs_.end(), normalized) == dirs_.end()) {
        dirs_.push_back(std::move(normalized));
    }
}

void ObservationBoundary::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirs_.clear();
}

bool ObservationBoundary::Contains(const std::filesystem::path& abs_path) const {
    // 名字口径:任一路径段叫 .evidence(大小写敏感——Windows 目录虽不分
    // 大小写,证据目录约定就是小写,不放大匹配面)。走原文即可,目录迭代
    // 给的就是盘上拼写。
    for (const auto& part : abs_path) {
        if (part == kEvidenceDirName) {
            return true;
        }
    }
    // 登记账口径:落在某枚已登记目录之下(含目录本身)。账空(常见情形:
    // 没开子代理日志)早退,不给每次 search 白做规范化。
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirs_.empty()) {
        return false;
    }
    // 查询侧过与登记侧同一道规范化:weakly_canonical 会把已存在前段解析
    // 成盘上真实拼写,两侧不同道就比不齐(env 拼写与盘上大小写常有出入)。
    const std::filesystem::path normalized = NormalizeAbsolute(abs_path);
    for (const std::filesystem::path& dir : dirs_) {
        auto prefix = dir.begin();
        auto it = normalized.begin();
        while (prefix != dir.end()) {
            if (it == normalized.end() || !SamePathComponent(*it, *prefix)) {
                break;
            }
            ++it;
            ++prefix;
        }
        if (prefix == dir.end()) {
            return true;
        }
    }
    return false;
}

bool PathInObservationBoundary(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    return ObservationBoundary::Instance().Contains(path);
}

std::string ObservationReadNotice(const std::filesystem::path& path, std::uintmax_t size_bytes) {
    if (!PathInObservationBoundary(path)) {
        return std::string();
    }
    std::string line = "[观察边界] " + PathToUtf8(path) + "(" + std::to_string(size_bytes) +
                       " 字节)是运行时观察记录(子代理/会话调试日志),内容含工具流,读它会把观察记录"
                       "再吞回上下文;";
    if (size_bytes > kObservationDiscourageThreshold) {
        line += "体积超过 256KB,建议不读,确要读请用 offset/limit 分段。\n";
    } else {
        line += "体积不大,已照常读取。\n";
    }
    return line;
}

}  // namespace lubancode::tools
