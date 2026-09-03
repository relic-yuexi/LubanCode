// PathComparisonKey 的实现(合同见 paths.hpp 的声明注释)。算法体自
// tools/isolation.cpp::NormalizeKey 原样搬来——与 runtime/worktree.cpp::
// NormalizeKey、tools/session_utils.cpp::NormalizePathForCompare 逐向量
// 相等(证据:tests/unit/platform/test_path_comparison_vectors.cpp)。
#include "platform/paths.hpp"

#include <algorithm>

namespace lubancode::platform {

std::string PathComparisonKey(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec || canonical.empty()) {
        canonical = path.lexically_normal();
    }
    std::string key = PathToUtf8(canonical);
    std::replace(key.begin(), key.end(), '\\', '/');
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    return key;
}

}  // namespace lubancode::platform
