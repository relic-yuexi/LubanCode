#include "tools/isolation.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace lubancode::tools {

namespace {

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 归一化比较键:weakly_canonical(失败退 lexically_normal),反斜杠统一
// 正斜杠,ASCII 折小写(Windows 习惯),尾斜杠剥掉。跟 agent/session_store
// 的 NormalizePathForCompare 同一套思路;这里不引 agent 层,单备一份。
std::string NormalizeKey(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
    if (ec || p.empty()) {
        p = path.lexically_normal();
    }
    std::string s = PathToUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

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
