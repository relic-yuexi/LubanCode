#include "tools/isolation.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "platform/paths.hpp"  // PathComparisonKey:比较键公共件(审计 P2 候选收编)

namespace lubancode::tools {

namespace {

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

}  // namespace

// 归一化比较键:合同与实现统一在 platform::PathComparisonKey(src 收口
// 审计 P2 候选:三处私房实现共享向量证一致后收编,这里只留领域薄名,
// 隔离闸内部与向量测试在用)。
std::string NormalizeKey(const std::filesystem::path& path) {
    return platform::PathComparisonKey(path);
}

namespace {

// key 是否落在 root_key 之下(root_key 自身不算,直接子级往下才算)。
bool IsUnder(const std::string& key, const std::string& root_key) {
    if (root_key.empty() || key.size() <= root_key.size()) {
        return false;
    }
    return key.compare(0, root_key.size(), root_key) == 0 && key[root_key.size()] == '/';
}

// 全部隔离栈操作的同一份 thread_local(藏在函数里,构造/析构顺序跟线程
// 走,不掺进程级静态析构的浑水)。注意不能在每个成员函数里各写一份
// thread_local——那是三个互不相干的栈。
std::vector<IsolationScope>& ScopeStack() {
    thread_local std::vector<IsolationScope> stack;
    return stack;
}

}  // namespace

void IsolationGuard::Push(IsolationScope scope) {
    ScopeStack().push_back(std::move(scope));
}

void IsolationGuard::Pop() {
    if (!ScopeStack().empty()) {
        ScopeStack().pop_back();
    }
}

const IsolationScope* IsolationGuard::Current() {
    return ScopeStack().empty() ? nullptr : &ScopeStack().back();
}

bool PathBlockedByIsolation(const std::string& utf8_path, const IsolationScope& scope) {
    if (scope.main_root.empty() || utf8_path.empty()) {
        return false;
    }
    // 相对路径按房解析(隔离子代理的路径基准是房,不是进程 cwd)。
    std::filesystem::path path = Utf8Path(utf8_path);
    if (path.is_relative()) {
        path = Utf8Path(scope.base_dir) / path;
    }
    const std::string key = NormalizeKey(path);
    const std::string main_key = NormalizeKey(Utf8Path(scope.main_root));
    if (main_key.empty()) {
        return false;
    }
    if (key == main_key) {
        return true;  // 主根本体
    }
    if (!IsUnder(key, main_key)) {
        return false;  // 主树之外(临时目录、别处),不归这道闸管
    }
    // 在主树之内:房区(main_root/.lubancode/worktrees/...)放行。
    const std::string worktrees_key = NormalizeKey(Utf8Path(scope.main_root) / ".lubancode" / "worktrees");
    return !IsUnder(key, worktrees_key);
}

}  // namespace lubancode::tools
