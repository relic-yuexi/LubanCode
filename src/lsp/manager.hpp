// LSP 会话总管:按语言养一批 lsp::Client,管三件事——
//   1) 路由:文件扩展名 -> 语言名(config lsp 段的 extensions 字段);
//   2) 懒启动:首次用到某语言才真起进程 + initialize 握手,起不来给人话
//      错误(附安装指路);
//   3) 闲置关停:每次 AcquireClient 记时间,下次取用前发现闲置超过
//      idle_minutes 就先关掉旧进程再拉一个新的(选的是"取用前检查"这条
//      实现最简单的路,不另起定时器线程);/lsp 命令看状态时也顺手收割
//      闲置进程,好让状态里能看到"已闲置关停"这一档。
// 会话结束(Manager 析构)把所有还活着的进程按 shutdown/exit + 2s 兜底杀
// 的规矩全关掉(lsp::Client::Shutdown 负责具体动作)。
//
// 线程模型:工具执行在 agent loop 里是严格串行的,slash 命令又只在两轮
// 对话之间跑,Manager 不会被并发调用,不加锁(跟 main.cpp 的
// McpServerRuntime 同一套假设)。
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "lsp/client.hpp"

namespace lubancode::lsp {

// 找不到命令时给人的安装指路。认识的服务器(clangd 等)给具体安装命令,
// 不认识的给通用提示。纯函数,单测直接盯。
std::string InstallHintForCommand(const std::string& command);

class Manager {
public:
    // root_path 是工作区根目录(通常是 cwd),initialize 的 rootUri 由它转来。
    Manager(std::map<std::string, config::LspServerConfig> configs, const std::string& root_path);
    ~Manager();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // 按扩展名路由:返回配置里管这个扩展的语言名;没人管返回 nullopt。
    // 扩展名比较大小写不敏感(Windows 下 .CPP 和 .cpp 是一个东西)。
    std::optional<std::string> LanguageForFile(const std::string& file_path) const;

    // 取(必要时先启动/重启)某语言的客户端。懒启动 + 闲置重启都在这里:
    // 进程没起过/已闲置关停/意外死掉 -> 起新进程 + initialize;起不来返回
    // 人话错误(附安装指路)。成功时更新最近使用时间。返回的裸指针由
    // Manager 持有,调用方不接管生命周期,别跨 Manager 生命周期存。
    std::expected<Client*, std::string> AcquireClient(const std::string& language);

    // /lsp 命令用的状态清单。顺手收割闲置进程(超时的先关掉,状态记成
    // "已闲置关停")。
    struct StatusEntry {
        std::string language;
        std::string command;
        std::string state;  // 未启动 / 运行中 / 已闲置关停 / 已退出
    };
    std::vector<StatusEntry> StatusList();

    // 会话结束:全部体面关停(shutdown/exit + 2s 兜底杀)。幂等。
    void ShutdownAll();

    // 仅供测试:把闲置阈值收窄到毫秒级,免得"验证闲置关停"要真等 10 分钟。
    void SetIdleMillisForTest(std::int64_t idle_ms);

private:
    enum class Phase { NotStarted, Running, IdleStopped };

    struct ServerState {
        config::LspServerConfig config;
        std::unique_ptr<Client> client;
        std::chrono::steady_clock::time_point last_use{};
        Phase phase = Phase::NotStarted;
    };

    std::int64_t IdleMillisFor(const ServerState& state) const;
    // 闲置检查:Running 且超过阈值就关停、标成 IdleStopped。
    void ReapIfIdle(ServerState& state);

    std::string root_uri_;
    std::map<std::string, ServerState> servers_;
    std::optional<std::int64_t> idle_override_ms_;
};

}  // namespace lubancode::lsp
