#include "lsp/manager.hpp"

#include <cctype>

namespace lubancode::lsp {

namespace {

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 取路径的扩展名(带点,小写)。没有扩展名返回空串。
std::string ExtensionOf(const std::string& file_path) {
    const std::size_t last_sep = file_path.find_last_of("/\\");
    const std::size_t name_begin = last_sep == std::string::npos ? 0 : last_sep + 1;
    const std::size_t dot = file_path.find_last_of('.');
    if (dot == std::string::npos || dot < name_begin) {
        return std::string();
    }
    return ToLowerAscii(file_path.substr(dot));
}

}  // namespace

std::string InstallHintForCommand(const std::string& command) {
    if (command.find("clangd") != std::string::npos) {
        return "未找到 clangd,可用 winget install LLVM.clangd 安装";
    }
    if (command.find("pyright") != std::string::npos) {
        return "未找到 " + command + ",可用 npm install -g pyright 安装";
    }
    if (command.find("pylsp") != std::string::npos) {
        return "未找到 " + command + ",可用 pip install python-lsp-server 安装";
    }
    return "请确认 " + command + " 已安装并在 PATH 里";
}

Manager::Manager(std::map<std::string, config::LspServerConfig> configs, const std::string& root_path)
    : root_uri_(PathToUri(root_path)) {
    for (auto& [language, config] : configs) {
        ServerState state;
        state.config = std::move(config);
        servers_.emplace(language, std::move(state));
    }
}

Manager::~Manager() {
    ShutdownAll();
}

void Manager::SetIdleMillisForTest(std::int64_t idle_ms) {
    idle_override_ms_ = idle_ms;
}

std::optional<std::string> Manager::LanguageForFile(const std::string& file_path) const {
    const std::string ext = ExtensionOf(file_path);
    if (ext.empty()) {
        return std::nullopt;
    }
    for (const auto& [language, state] : servers_) {
        for (const auto& configured : state.config.extensions) {
            if (ToLowerAscii(configured) == ext) {
                return language;
            }
        }
    }
    return std::nullopt;
}

std::int64_t Manager::IdleMillisFor(const ServerState& state) const {
    if (idle_override_ms_.has_value()) {
        return *idle_override_ms_;
    }
    return static_cast<std::int64_t>(state.config.idle_minutes) * 60 * 1000;
}

void Manager::ReapIfIdle(ServerState& state) {
    if (state.phase != Phase::Running || state.client == nullptr) {
        return;
    }
    const auto idle =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - state.last_use)
            .count();
    if (idle > IdleMillisFor(state)) {
        state.client->Shutdown();
        state.client.reset();
        state.phase = Phase::IdleStopped;
    }
}

std::expected<Client*, std::string> Manager::AcquireClient(const std::string& language) {
    auto it = servers_.find(language);
    if (it == servers_.end()) {
        return std::unexpected("config 的 lsp 段里没有语言 " + language + " 的配置");
    }
    ServerState& state = it->second;

    // 闲置超时的旧进程先收掉,下面按"没有进程"重新拉起。
    ReapIfIdle(state);

    // 进程意外死掉(崩了/被人杀了)也按重启处理,不让一具尸体挡路。
    if (state.client != nullptr && !state.client->Alive()) {
        state.client->Shutdown();
        state.client.reset();
    }

    if (state.client == nullptr) {
        auto client = std::make_unique<Client>(language);
        const auto start_result = client->StartProcess(state.config.command, state.config.args);
        if (!start_result.success) {
            std::string error = "启动 " + language + " 的 LSP 服务器失败: " + start_result.error;
            // 找不到可执行文件是最常见的一档,指路安装。
            if (start_result.error.find("未找到") != std::string::npos) {
                error += "。" + InstallHintForCommand(state.config.command);
            }
            return std::unexpected(error);
        }
        const auto init_result = client->Initialize(root_uri_);
        if (!init_result.has_value()) {
            client->Shutdown();
            return std::unexpected("LSP 服务器 " + language + " 握手失败: " + init_result.error());
        }
        state.client = std::move(client);
        state.phase = Phase::Running;
    }

    state.last_use = std::chrono::steady_clock::now();
    return state.client.get();
}

std::vector<Manager::StatusEntry> Manager::StatusList() {
    std::vector<StatusEntry> out;
    for (auto& [language, state] : servers_) {
        ReapIfIdle(state);
        StatusEntry entry;
        entry.language = language;
        entry.command = state.config.command;
        switch (state.phase) {
            case Phase::NotStarted:
                entry.state = "未启动";
                break;
            case Phase::Running:
                entry.state = (state.client != nullptr && state.client->Alive()) ? "运行中" : "已退出";
                break;
            case Phase::IdleStopped:
                entry.state = "已闲置关停";
                break;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

void Manager::ShutdownAll() {
    for (auto& [language, state] : servers_) {
        if (state.client != nullptr) {
            state.client->Shutdown();
            state.client.reset();
        }
        if (state.phase == Phase::Running) {
            state.phase = Phase::NotStarted;
        }
    }
}

}  // namespace lubancode::lsp
