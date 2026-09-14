// assistant_host.hpp 的实现。装配序(§四):
//   1. 解析参数根/状态根/profile;配置装载 + 活配置快照(首配模型后
//      新会话吃到新配置);
//   2. LocalWebServer 起监听(资源门/端口门先过)→ WebAuthService 装门;
//   3. 拿 profile 锁(此时端口已知,锁账写全;撞活实例→验明正身只开
//      它的页面,不杀原进程);
//   4. app_server::Server(助理模式 Detached + 扩展方法面)就绪;
//   5. 接口健康了才开浏览器(--no-open 只打印 URL);
//   6. 主循环:串行服务控制连接(单控制连接 + 显式接管,§六),Ctrl+C/
//      exit 走宽限收口,释放监听与锁。
#include "app/assistant_host.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <share.h>  // _wfsopen 的 _SH_DENYNO
#endif

#include <nlohmann/json.hpp>

#include "app/backend_stack.hpp"
#include "app/cli_options.hpp"
#include "app/version.hpp"
#include "app_server/connection_snapshot.hpp"
#include "app_server/dispatcher.hpp"
#include "app_server/local_web_server.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "app_server/session_assembly.hpp"
#include "config/config.hpp"
#include "gateway/profile.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::app {

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 随机凭据(bootstrap/会话/控制口):32 字节十六进制。random_device 是
// 本地一次性配对凭据的合适来源——不是长期密钥,不是跨机秘密。
std::string GenerateSecretHex() {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::random_device source;
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const unsigned byte = static_cast<unsigned>(source()) & 0xFFu;
        out.push_back(kDigits[(byte >> 4) & 0xF]);
        out.push_back(kDigits[byte & 0xF]);
    }
    return out;
}

std::string MakeBootId() {
    return "asst-" + GenerateSecretHex().substr(0, 16);
}

std::int64_t WallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// WebAuthService:一次性 bootstrap 凭据 + 会话 cookie(§七)。
// 线程安全(accept 线程与控制口回调并发碰)。
// ---------------------------------------------------------------------------

class WebAuthService {
public:
    WebAuthService(std::int64_t bootstrap_ttl_ms, std::int64_t session_ttl_ms)
        : bootstrap_ttl_ms_(bootstrap_ttl_ms), session_ttl_ms_(session_ttl_ms) {}

    const std::string& control_secret() const { return control_secret_; }

    // 铸一张新的 bootstrap URL(一次性凭据只进 URL fragment,永不进服务
    // 端访问日志——fragment 根本不发到服务端,这里只发放在内存账里)。
    // 悬着的 bootstrap 有帽,旧的自然过期淘汰,不无限攒。
    std::string MakeBootstrapUrl(int port) {
        const std::string secret = GenerateSecretHex();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back({secret, SteadyMs() + bootstrap_ttl_ms_});
            while (pending_.size() > 8) {
                pending_.pop_front();
            }
        }
        return "http://127.0.0.1:" + std::to_string(port) + "/#b=" + secret;
    }

    // 一次性消费:恒时比较、限时效、用过即焚(防重放)。任何一路不过都
    // 回 nullopt——话面不区分"没带/带错/过期/用过",不给试探省事。
    std::optional<std::string> ConsumeBootstrap(const std::string& given) {
        if (given.empty() || given.size() > 256) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t now = SteadyMs();
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (app_server::WebConstantTimeEqual(given, it->secret)) {
                const bool expired = now > it->expires_at_ms;
                pending_.erase(it);  // 过期/用过都焚
                if (expired) {
                    return std::nullopt;
                }
                const std::string session = GenerateSecretHex();
                sessions_[session] = now + session_ttl_ms_;
                PruneSessionsLocked(now);
                return session;
            }
        }
        return std::nullopt;
    }

    bool ValidateSession(const std::string& value) {
        if (value.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t now = SteadyMs();
        PruneSessionsLocked(now);
        const auto found = sessions_.find(value);
        return found != sessions_.end() && now <= found->second;
    }

    // 控制口凭据校验(锁文件里的 control_secret;恒时比较)。
    bool ValidateControlSecret(const std::string& given) const {
        return app_server::WebConstantTimeEqual(given, control_secret_);
    }

private:
    static std::int64_t SteadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void PruneSessionsLocked(std::int64_t now) {
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (now > it->second) {
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    struct SecretEntry {
        std::string secret;
        std::int64_t expires_at_ms;
    };

    mutable std::mutex mutex_;
    std::deque<SecretEntry> pending_;
    std::map<std::string, std::int64_t> sessions_;
    std::int64_t bootstrap_ttl_ms_;
    std::int64_t session_ttl_ms_;
    std::string control_secret_ = GenerateSecretHex();
};

// ---------------------------------------------------------------------------
// 助理实例锁(§四:重复启动/探测失败/锁不明的裁决)。
// 锁账:JSON {schema,pid,bootId,port,controlSecret,startedAtMs}。
// 独占靠 create-new("x" 模式)原子占位——与 GatewayLock 同一把尺;账面
// 字段是助理自己的(port/controlSecret),不复用 GatewayLockRecord。
// ---------------------------------------------------------------------------

struct AssistantLockRecord {
    unsigned long pid = 0;
    std::string boot_id;
    int port = 0;
    std::string control_secret;
    std::int64_t started_at_ms = 0;

    nlohmann::json ToJson() const {
        return nlohmann::json{{"schema", 1},
                              {"pid", pid},
                              {"bootId", boot_id},
                              {"port", port},
                              {"controlSecret", control_secret},
                              {"startedAtMs", started_at_ms}};
    }

    // 读侧容错:缺 pid/bootId/port 的账按"不完整"交回(调用方裁决),坏
    // JSON 按"读不懂"处理。
    enum class ParseStatus { Ok, Incomplete, Unreadable };
    static ParseStatus Parse(const std::string& text, AssistantLockRecord& out) {
        const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            return ParseStatus::Unreadable;
        }
        if (!json.contains("pid") || !json["pid"].is_number_unsigned() ||
            !json.contains("bootId") || !json["bootId"].is_string()) {
            return ParseStatus::Incomplete;
        }
        out = AssistantLockRecord();
        out.pid = json["pid"].get<unsigned long>();
        out.boot_id = json["bootId"].get<std::string>();
        if (json.contains("port") && json["port"].is_number_integer()) {
            out.port = json["port"].get<int>();
        }
        if (json.contains("controlSecret") && json["controlSecret"].is_string()) {
            out.control_secret = json["controlSecret"].get<std::string>();
        }
        if (json.contains("startedAtMs") && json["startedAtMs"].is_number_integer()) {
            out.started_at_ms = json["startedAtMs"].get<std::int64_t>();
        }
        if (out.port <= 0) {
            return ParseStatus::Incomplete;
        }
        return ParseStatus::Ok;
    }
};

class AssistantLock {
public:
    AssistantLock() = default;
    ~AssistantLock() { Release(); }
    AssistantLock(const AssistantLock&) = delete;
    AssistantLock& operator=(const AssistantLock&) = delete;

    struct Outcome {
        enum class Status { Acquired, AliveHolder, BrokenLock, IoError };
        Status status = Status::IoError;
        AssistantLockRecord holder;
        std::string detail;
    };

    // 取锁(实例方法:成功后本对象持锁,析构释放)。
    Outcome TryAcquire(const std::filesystem::path& file, const AssistantLockRecord& self) {
        Release();
        Outcome outcome;
        for (int attempt = 0; attempt < 5; ++attempt) {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec && !file.parent_path().empty()) {
                outcome.detail = "建锁目录失败: " + ec.message();
                return outcome;
            }
            std::FILE* handle = nullptr;
#ifdef _WIN32
            handle = _wfsopen(file.c_str(), L"wbx", _SH_DENYNO);
#else
            handle = std::fopen(file.c_str(), "wbx");
#endif
            if (handle != nullptr) {
                const std::string text = self.ToJson().dump();
                const bool wrote = std::fwrite(text.data(), 1, text.size(), handle) == text.size() &&
                                   std::fflush(handle) == 0;
                std::fclose(handle);
                if (!wrote) {
                    std::filesystem::remove(file, ec);
                    outcome.detail = "锁账写不进";
                    return outcome;
                }
                file_ = file;
                outcome.status = Outcome::Status::Acquired;
                return outcome;
            }
            // 占位撞上:读账核身份。读不懂 = 保守拒(不删不抢);持有者
            // 死透 = 陈旧,清掉重试;活着 = AliveHolder(调用方探健康)。
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                outcome.status = Outcome::Status::BrokenLock;
                outcome.detail = "锁文件在但读不开(权限?): " + platform::PathToUtf8(file);
                return outcome;
            }
            std::stringstream buffer;
            buffer << in.rdbuf();
            AssistantLockRecord existing;
            const AssistantLockRecord::ParseStatus parse =
                AssistantLockRecord::Parse(buffer.str(), existing);
            if (parse != AssistantLockRecord::ParseStatus::Ok) {
                outcome.status = Outcome::Status::BrokenLock;
                outcome.detail = parse == AssistantLockRecord::ParseStatus::Unreadable
                                     ? "锁账读不懂,保守不动: " + platform::PathToUtf8(file)
                                     : "锁账不完整(实例可能在启动窗口里),稍候重试: " +
                                           platform::PathToUtf8(file);
                return outcome;
            }
            if (platform::IsProcessAlive(existing.pid)) {
                outcome.status = Outcome::Status::AliveHolder;
                outcome.holder = existing;
                return outcome;
            }
            std::filesystem::remove(file, ec);  // 陈旧锁,清掉有界重试
        }
        outcome.detail = "锁竞争重试撞满(陈旧锁清了又被占?)";
        return outcome;
    }

    void Release() {
        if (!file_.empty()) {
            std::error_code ec;
            std::filesystem::remove(file_, ec);
            file_.clear();
        }
    }

private:
    std::filesystem::path file_;
};

// ---------------------------------------------------------------------------
// 跨进程探测(重复启动分叉):/healthz 对 bootId、/control/open 拿新 URL。
// 走 net::ConnectTcp + 手写 HTTP/1.1(回环一次性请求,Connection: close)。
// ---------------------------------------------------------------------------

struct HttpProbeResult {
    int status = 0;  // 0 = 连不上/读不回
    std::string body;
};

HttpProbeResult HttpOnce(int port, const std::string& method, const std::string& target,
                         const std::string& body) {
    HttpProbeResult result;
    std::string error;
    app_server::net::Socket socket = app_server::net::ConnectTcp("127.0.0.1", port, error);
    if (!socket.valid()) {
        return result;
    }
    socket.SetRecvTimeoutMs(3000);
    std::string request;
    request += method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    if (!body.empty()) {
        request += "Content-Type: text/plain; charset=utf-8\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    request += body;
    if (!socket.SendAll(request)) {
        return result;
    }
    std::string response;
    char buffer[4096];
    while (response.size() <= 64 * 1024) {
        const long got = socket.Recv(buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        response.append(buffer, buffer + got);
        // Connection: close:读空即齐;body 齐不齐由调用方按 Content-Length
        // 或 JSON 可解性裁。
    }
    if (response.rfind("HTTP/1.1 ", 0) == 0 && response.size() > 12) {
        result.status = std::atoi(response.c_str() + 9);
    }
    const std::size_t split = response.find("\r\n\r\n");
    if (split != std::string::npos) {
        result.body = response.substr(split + 4);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 浏览器打开(监听就绪后才调;失败不拦启动——URL 已打印可复制)。
// ---------------------------------------------------------------------------

void OpenBrowserFor(const std::string& url) {
#ifdef _WIN32
    // cmd start 的空标题参数挡住"URL 被当窗口标题"的老坑。
    const platform::ProcessResult result =
        platform::RunProcess({"cmd.exe", "/d", "/c", "start", "", url}, 8000);
    if (result.spawn_failed || result.exit_code != 0) {
        std::fprintf(stderr,
                     "[assistant] 打开浏览器失败,请手动复制上面的地址(%s)\n",
                     result.spawn_error.empty() ? "start 退出码非零" : result.spawn_error.c_str());
    }
#else
    for (const char* opener : {"xdg-open", "open"}) {
        const platform::ProcessResult result = platform::RunProcess({opener, url}, 8000);
        if (!result.spawn_failed && result.exit_code == 0) {
            return;
        }
    }
    std::fprintf(stderr, "[assistant] 打开浏览器失败(xdg-open/open 都不行),请手动复制上面的地址\n");
#endif
}

// ---------------------------------------------------------------------------
// 活配置快照:首配模型后,新 thread 的装配吃到新配置(§六 config 族:
// "区分需重启与新会话生效"——本进程不改已开 thread 的冻结材料,新场吃
// 新账)。线程安全:方法面在读线程,装配工厂在 thread/start(同一条
// 读线程)——但配置方法与装配可能被不同连接碰,仍按共享件加锁。
// ---------------------------------------------------------------------------

class AssistantConfigState {
public:
    explicit AssistantConfigState(config::ConfigResult initial) : current_(std::move(initial)) {}

    void Update(config::ConfigResult next) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::move(next);
    }

    config::ConfigResult Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

private:
    mutable std::mutex mutex_;
    config::ConfigResult current_;
};

// ---------------------------------------------------------------------------
// 助理扩展方法面(§六:方法族先复用已有,缺口定名再接线;全部 additive,
// 只在助理模式的方法注册器里挂,独立 app-server 不见)。
// 凭据纪律(§五/§七):永不回显密钥,错误话里也不带。
// ---------------------------------------------------------------------------

constexpr const char* kMethodAssistantStatus = "assistant/status";
constexpr const char* kMethodConfigStatus = "config/status";
constexpr const char* kMethodConfigModelSet = "config/model/set";
constexpr const char* kMethodConfigTest = "config/test";

struct AssistantIdentity {
    std::string boot_id;
    std::string profile;
    int port = 0;
    std::string cwd;
    std::int64_t started_at_ms = 0;
};

nlohmann::json BuildConfigStatus(const config::ConfigResult& config_result) {
    const config::Config& config = config_result.config;
    const bool configured = config::RequireConfigured(config_result).has_value();
    const config::ProviderConfig* provider =
        config::FindProvider(config.providers, config.active_provider);
    // 密钥只报"有没有、从哪来",绝不回值(inline 打码也不打——根本不回)。
    std::string key_source = "none";
    if (config.auth_mode == config::ProviderAuthMode::Inline) {
        key_source = "inline";
    } else if (config.auth_mode == config::ProviderAuthMode::Env) {
        key_source = "env:" + (provider != nullptr && !provider->key_env.empty()
                                   ? provider->key_env
                                   : std::string("LUBAN_API_KEY"));
    }
    const bool key_configured = !config.auth_token.empty();
    return nlohmann::json{{"configured", configured},
                          {"provider", config.active_provider},
                          {"model", config.model},
                          {"wire", config::ProviderWireName(config.wire)},
                          {"baseUrl", config.base_url},
                          {"apiKeyConfigured", key_configured},
                          {"apiKeySource", key_source}};
}

void RegisterAssistantMethods(app_server::Dispatcher& dispatcher,
                              const std::shared_ptr<AssistantConfigState>& config_state,
                              const AssistantIdentity& identity) {
    // assistant/status:实例健康快照(页头用;停止助理走既有 shutdown)。
    dispatcher.RegisterMethod(
        kMethodAssistantStatus,
        [identity](const app_server::IncomingRequest& request, app_server::DispatchContext&)
            -> std::optional<nlohmann::json> {
            if (!request.params.is_object()) {
                return app_server::MakeError(request.id, app_server::kErrInvalidParams,
                                             "assistant/status: params 须是对象");
            }
            nlohmann::json result = nlohmann::json::object();
            result["bootId"] = identity.boot_id;
            result["profile"] = identity.profile;
            result["port"] = identity.port;
            result["cwd"] = identity.cwd;
            result["lubancodeVersion"] = std::string(kVersion);
            result["workLifetime"] = "detached";
            result["uptimeMs"] = WallClockMs() - identity.started_at_ms;
            return app_server::MakeResult(request.id, std::move(result));
        });

    // config/status:当前生效配置投影(零密钥)。
    dispatcher.RegisterMethod(
        kMethodConfigStatus,
        [config_state](const app_server::IncomingRequest& request, app_server::DispatchContext&)
            -> std::optional<nlohmann::json> {
            if (!request.params.is_object()) {
                return app_server::MakeError(request.id, app_server::kErrInvalidParams,
                                             "config/status: params 须是对象");
            }
            return app_server::MakeResult(request.id, BuildConfigStatus(config_state->Snapshot()));
        });

    // config/model/set:定点保存模型配置(provider 名缺省 "assistant")。
    // 保存与连接检查分开(§五:"测试连接失败与配置保存成功分开显示")。
    dispatcher.RegisterMethod(
        kMethodConfigModelSet,
        [config_state](const app_server::IncomingRequest& request, app_server::DispatchContext&)
            -> std::optional<nlohmann::json> {
            const nlohmann::json& params = request.params;
            if (!params.is_object()) {
                return app_server::MakeError(request.id, app_server::kErrInvalidParams,
                                             "config/model/set: params 须是对象");
            }
            const auto read_string = [&params](const char* key) -> std::string {
                if (params.contains(key) && params[key].is_string()) {
                    return params[key].get<std::string>();
                }
                return std::string();
            };
            config::ProviderConfig provider;
            provider.name = read_string("name");
            if (provider.name.empty()) {
                provider.name = "assistant";
            }
            provider.base_url = read_string("baseUrl");
            provider.model = read_string("model");
            const std::string wire_name = read_string("wire");
            if (!wire_name.empty()) {
                const auto wire = config::ParseProviderWire(wire_name);
                if (!wire.has_value()) {
                    return app_server::MakeError(
                        request.id, app_server::kErrInvalidParams,
                        "config/model/set: wire 认不得 \"" + wire_name +
                            "\"(认 anthropic|responses|chat|google-generate-content)");
                }
                provider.wire = *wire;
            }
            const std::string api_key = read_string("apiKey");
            const std::string key_env = read_string("keyEnv");
            if (!api_key.empty()) {
                provider.auth = config::ProviderAuthMode::Inline;
                provider.api_key = api_key;
            } else if (!key_env.empty()) {
                provider.auth = config::ProviderAuthMode::Env;
                provider.key_env = key_env;
            } else {
                return app_server::MakeError(request.id, app_server::kErrInvalidParams,
                                             "config/model/set: apiKey 与 keyEnv 至少给一个");
            }
            if (provider.base_url.empty() || provider.model.empty()) {
                return app_server::MakeError(request.id, app_server::kErrInvalidParams,
                                             "config/model/set: baseUrl 与 model 都要有");
            }
            // 保存:同名已存在走 Replace(再配置),否则 Add;然后置 active。
            const config::ConfigResult snapshot = config_state->Snapshot();
            const bool exists =
                config::FindProvider(snapshot.config.providers, provider.name) != nullptr;
            auto saved = exists ? config::ReplaceProviderInGlobalConfig(provider.name, provider)
                                : config::AddProviderToGlobalConfig(provider);
            if (!saved.has_value()) {
                return app_server::MakeError(request.id, app_server::kErrInternalError,
                                             "config/model/set: 保存失败——" + saved.error(),
                                             nlohmann::json{{"code", "config_write_failed"}});
            }
            const auto activated = config::SetActiveProviderInGlobalConfig(provider.name);
            if (!activated.has_value()) {
                return app_server::MakeError(request.id, app_server::kErrInternalError,
                                             "config/model/set: 保存了但激活失败——" +
                                                 activated.error(),
                                             nlohmann::json{{"code", "config_write_failed"}});
            }
            // 活配置换血:新开 thread 吃新账;已开 thread 的材料不动(冻结
            // 合同,§六——改配置不追改在途请求)。
            const auto reloaded = config::LoadFromEnv();
            if (reloaded.has_value()) {
                config_state->Update(*reloaded);
            }
            nlohmann::json result = nlohmann::json::object();
            result["saved"] = true;
            result["provider"] = provider.name;
            result["model"] = provider.model;
            result["savedTo"] = *saved;
            return app_server::MakeResult(request.id, std::move(result));
        });

    // config/test:端点连通检查(如实:只验 TCP 可达,不冒充"鉴权通过")。
    // 有界:探测线程 + 8 秒 deadline,读线程不被慢 DNS/黑洞 IP 吊死。
    dispatcher.RegisterMethod(
        kMethodConfigTest,
        [config_state](const app_server::IncomingRequest& request, app_server::DispatchContext&)
            -> std::optional<nlohmann::json> {
            const config::ConfigResult snapshot = config_state->Snapshot();
            const std::string& base_url = snapshot.config.base_url;
            if (base_url.rfind("http://", 0) != 0 && base_url.rfind("https://", 0) != 0) {
                return app_server::MakeResult(
                    request.id, nlohmann::json{{"reachable", false},
                                               {"detail", "baseUrl 未配置或不合法(先保存模型配置)"}});
            }
            std::string rest = base_url.substr(base_url.find("://") + 3);
            const std::size_t slash = rest.find('/');
            const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
            std::string host = host_port;
            int port = base_url.rfind("https://", 0) == 0 ? 443 : 80;
            const std::size_t colon = host_port.rfind(':');
            if (colon != std::string::npos) {
                host = host_port.substr(0, colon);
                port = std::atoi(host_port.c_str() + colon + 1);
            }
            if (host.empty() || port <= 0) {
                return app_server::MakeResult(request.id,
                                              nlohmann::json{{"reachable", false},
                                                             {"detail", "baseUrl 主机/端口解析不出"}});
            }
            std::future<bool> probe = std::async(std::launch::async, [host, port]() {
                std::string error;
                app_server::net::Socket socket = app_server::net::ConnectTcp(host, port, error);
                return socket.valid();
            });
            const std::future_status status = probe.wait_for(std::chrono::seconds(8));
            nlohmann::json result = nlohmann::json::object();
            if (status == std::future_status::timeout) {
                // async 的探测线程可能还吊在 connect 上——future 析构会等它;
                // 这里到点就答,不等(线程由运行时收)。
                result["reachable"] = false;
                result["detail"] = "8 秒内连不上 " + host + ":" + std::to_string(port);
            } else {
                const bool reachable = probe.get();
                result["reachable"] = reachable;
                result["detail"] = reachable ? "TCP 可达(只验端点连通,不验鉴权与模型)"
                                             : "连不上 " + host + ":" + std::to_string(port);
            }
            return app_server::MakeResult(request.id, std::move(result));
        });
}

// initialize 结果的助理扩展(能力声明区分,W0):additive 字段,老前端
// 零感知;独立 app-server 的 initialize 不带这些。
nlohmann::json ExtendInitializeResult(nlohmann::json result) {
    if (!result.contains("capabilities") || !result["capabilities"].is_object()) {
        result["capabilities"] = nlohmann::json::object();
    }
    nlohmann::json& capabilities = result["capabilities"];
    capabilities["mode"] = "assistant";
    capabilities["workLifetime"] = "detached";
    if (!capabilities.contains("methods") || !capabilities["methods"].is_array()) {
        capabilities["methods"] = nlohmann::json::array();
    }
    capabilities["methods"].push_back(kMethodAssistantStatus);
    capabilities["methods"].push_back(kMethodConfigStatus);
    capabilities["methods"].push_back(kMethodConfigModelSet);
    capabilities["methods"].push_back(kMethodConfigTest);
    return result;
}

// ---------------------------------------------------------------------------
// 信号:Ctrl+C 宽限收口(§四)。第一阶段进程随终端退出本就允许,这里
// 给手动 Ctrl+C 一条干净路:置旗→停收新活→收口→落账→释放监听与锁。
// ---------------------------------------------------------------------------

std::atomic<bool>& AssistantStopFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

void HandleAssistantSignal(int) {
    AssistantStopFlag().store(true);
}

}  // namespace

int RunAssistantMode(const AssistantCliArgs& args) {
    // ---- 1. 根与参数裁决(§四) ----
    const auto state_root = config::StateRootDir();
    if (!state_root.has_value()) {
        std::fprintf(stderr, "assistant: 状态根不可用(应用根变量坏或找不到主目录)\n");
        return 1;
    }
    const std::string profile = args.profile.empty() ? std::string("default") : args.profile;
    if (!gateway::IsValidGatewayProfileName(profile)) {
        std::fprintf(stderr, "assistant: profile 名须是单段名(不带路径): %s\n", profile.c_str());
        return 1;
    }
    const std::filesystem::path profile_dir =
        tools::Utf8ToPath(*state_root) / "assistant" / tools::Utf8ToPath(profile);
    const std::filesystem::path lock_file = profile_dir / "assistant.lock";

    const std::string boot_id = MakeBootId();
    const std::int64_t started_at_ms = WallClockMs();

    // ---- 2. 配置装载(自己的路,与 gateway run 同款;缺模型不拦启动——
    // 首配流程在页内走,§五) ----
    const auto config_result = config::LoadFromEnv();
    if (!config_result.has_value()) {
        std::fprintf(stderr, "assistant: 配置装载失败——%s\n", config_result.error().c_str());
        return 1;
    }
    auto config_state = std::make_shared<AssistantConfigState>(*config_result);

    // ---- 3. 随包网页资源定位(发布包:exe 旁 web/assistant;开发/测试:
    // LUBANCODE_ASSISTANT_WEB 指源码树副本;再退当前目录) ----
    std::filesystem::path assets_root;
    if (const char* env_web = std::getenv("LUBANCODE_ASSISTANT_WEB");
        env_web != nullptr && *env_web != '\0') {
        assets_root = tools::Utf8ToPath(env_web);
    } else {
        if (const auto executable = platform::ExecutablePath(); executable.has_value()) {
            assets_root = executable->parent_path() / "web" / "assistant";
        } else {
            assets_root = std::filesystem::path("web") / "assistant";
        }
    }

    // ---- 4. WebAuthService + LocalWebServer(先起监听;端口在锁账里要
    // 写全,所以监听先于拿锁;锁撞活实例时这里的监听随进程退出释放) ----
    auto web_auth = std::make_shared<WebAuthService>(2 * 60 * 1000, 24 * 60 * 60 * 1000);
    // 端口汇:系统分配的端口要 Start 之后才知道,而 handle_open 闭包构造
    // 在前——闭包捕这个汇,Start 后填值。
    const std::shared_ptr<std::atomic<int>> port_sink = std::make_shared<std::atomic<int>>(0);
    app_server::LocalWebOptions web_options;
    web_options.port = args.port;
    web_options.assets_root = assets_root;
    web_options.manifest = app_server::AssistantWebManifest();
    web_options.validate_session = [web_auth](const std::string& value) {
        return web_auth->ValidateSession(value);
    };
    web_options.consume_bootstrap = [web_auth](const std::string& secret) {
        return web_auth->ConsumeBootstrap(secret);
    };
    web_options.handle_open = [web_auth, port_sink](
                                  const std::string& secret) -> std::optional<nlohmann::json> {
        if (!web_auth->ValidateControlSecret(secret)) {
            return std::nullopt;
        }
        const int port = port_sink->load();
        if (port <= 0) {
            return std::nullopt;  // 服务还没就绪(理论到不了,防御)
        }
        return nlohmann::json{{"url", web_auth->MakeBootstrapUrl(port)}};
    };
    web_options.health_body = [boot_id, profile]() {
        return nlohmann::json{{"ok", true},
                              {"service", "lubancode-assistant"},
                              {"bootId", boot_id},
                              {"profile", profile},
                              {"version", std::string(kVersion)},
                              {"pid", platform::CurrentProcessId()}};
    };
    if (const auto root = config::StateRootDir(); root.has_value()) {
        web_options.artifact_dir = *root + "/browser-artifacts";
    }
    const std::string artifact_dir = web_options.artifact_dir;  // move 前留底

    app_server::LocalWebServer web_server(std::move(web_options));
    if (!web_server.Start()) {
        std::fprintf(stderr, "assistant: %s\n", web_server.last_error().c_str());
        return 1;
    }
    port_sink->store(web_server.actual_port());

    // ---- 5. 拿 profile 锁(端口已知,锁账写全) ----
    AssistantLock lock;
    {
        AssistantLockRecord self;
        self.pid = platform::CurrentProcessId();
        self.boot_id = boot_id;
        self.port = web_server.actual_port();
        self.control_secret = web_auth->control_secret();
        self.started_at_ms = started_at_ms;
        const AssistantLock::Outcome lock_outcome = lock.TryAcquire(lock_file, self);
        if (lock_outcome.status == AssistantLock::Outcome::Status::AliveHolder) {
            // 撞活实例:验明正身(/healthz 的 bootId 对锁账)→控制口要新
            // 页面 URL→开页面→退。探测失败/锁不明:报错,不杀原进程、不
            // 复用可疑 URL。
            web_server.Stop();
            const AssistantLockRecord& holder = lock_outcome.holder;
            const HttpProbeResult health = HttpOnce(holder.port, "GET", "/healthz", "");
            nlohmann::json body = nlohmann::json::parse(health.body, nullptr, false);
            const bool verified = health.status == 200 && !body.is_discarded() && body.is_object() &&
                                  body.contains("bootId") && body["bootId"].is_string() &&
                                  body["bootId"].get<std::string>() == holder.boot_id;
            if (!verified) {
                std::fprintf(stderr,
                             "assistant: profile \"%s\" 已有实例持锁(pid %lu),但健康探测没对上"
                             "身份——不杀原进程、不复用可疑 URL。人工确认后处置锁文件:\n  %s\n",
                             profile.c_str(), holder.pid, platform::PathToUtf8(lock_file).c_str());
                return 3;
            }
            if (holder.control_secret.empty()) {
                std::fprintf(stderr,
                             "assistant: 旧实例(pid %lu)健康,但锁账缺控制口凭据——开不了它的"
                             "新页面;请直接用旧实例启动时打印的地址\n",
                             holder.pid);
                return 3;
            }
            const HttpProbeResult open =
                HttpOnce(holder.port, "POST", "/control/open", holder.control_secret);
            nlohmann::json open_body = nlohmann::json::parse(open.body, nullptr, false);
            if (open.status != 200 || open_body.is_discarded() || !open_body.is_object() ||
                !open_body.contains("url") || !open_body["url"].is_string()) {
                std::fprintf(stderr,
                             "assistant: 旧实例(pid %lu)健康,但控制口要不到新页面地址——不复用"
                             "可疑 URL。请直接用旧实例启动时打印的地址\n",
                             holder.pid);
                return 3;
            }
            const std::string url = open_body["url"].get<std::string>();
            std::fprintf(stderr, "assistant: profile \"%s\" 已在运行(pid %lu),打开它的页面\n",
                         profile.c_str(), holder.pid);
            if (!args.no_open) {
                OpenBrowserFor(url);
            } else {
                std::fprintf(stderr, "assistant: --no-open 下不自动开浏览器,它的页面地址:\n  %s\n",
                             url.c_str());
            }
            return 2;
        }
        if (lock_outcome.status != AssistantLock::Outcome::Status::Acquired) {
            web_server.Stop();
            std::fprintf(stderr, "assistant: 拿不到实例锁(%s)\n  %s\n",
                         lock_outcome.status == AssistantLock::Outcome::Status::BrokenLock
                             ? "锁账不明"
                             : "IO 错",
                         lock_outcome.detail.c_str());
            return 3;
        }
    }

    // ---- 6. AppServer(助理模式) ----
    app_server::ServerOptions server_options;
    server_options.workspaces_dir = *state_root + "/workspaces";
    server_options.cwd = platform::CurrentDirUtf8();
    switch (config_result->config.wire) {
        case config::Wire::Anthropic:
            server_options.session_wire = "anthropic";
            break;
        case config::Wire::Responses:
            server_options.session_wire = "responses";
            break;
        case config::Wire::ChatCompletions:
            server_options.session_wire = "chat";
            break;
        case config::Wire::GoogleGenerateContent:
            server_options.session_wire = "google-generate-content";
            break;
    }
    server_options.session_model = config_result->config.model;
    // 助理模式不设 connection_snapshot:首配流程会改配置,启动冻结的快照
    // 会误导;当前连接真值走 config/status(活账)。
    server_options.session_provider =
        config::BoundProviderName(config_result->config, config_result->config.active_provider);
    server_options.max_steps_per_turn = app_server::ResolveMaxStepsPerTurn(*config_result);
    // W0 断线合同:Detached——关标签/刷新/断 WS 只撤订阅,不取消已受理
    // 工作;停止任务是 turn/interrupt,停止助理是 shutdown。
    server_options.work_lifetime = app_server::ServerOptions::WorkLifetime::Detached;
    // 活配置装配:一场 thread 一次,吃当时的快照(首配后新场用新账)。
    // 无部署档 = 显式零工具默认档(与 RunAppServerMode 同规矩;助理的
    // 工具面扩编是后续批次,不冒充)。
    server_options.assembly_factory = [config_state]() {
        auto snapshot = std::make_shared<config::Config>(config_state->Snapshot().config);
        app_server::SessionAssemblyRequest request;
        request.config = snapshot.get();
        request.backend_factory = [snapshot]() { return BuildBackend(*snapshot); };
        request.system_prompt = app_server::kAppServerDefaultSystemPrompt;
        request.max_steps_per_turn = app_server::ResolveMaxStepsPerTurn(config_state->Snapshot());
        return app_server::AssembleSession(std::move(request));
    };
    const AssistantIdentity identity{boot_id, profile, web_server.actual_port(),
                                     platform::CurrentDirUtf8(), started_at_ms};
    server_options.extra_method_registrar =
        [config_state, identity](app_server::Dispatcher& dispatcher) {
            RegisterAssistantMethods(dispatcher, config_state, identity);
        };
    server_options.initialize_result_extender = [](nlohmann::json result) {
        return ExtendInitializeResult(std::move(result));
    };
    server_options.browser_artifact_dir = artifact_dir;

    app_server::Server server(
        std::move(server_options),
        [config_state]() { return BuildBackend(config_state->Snapshot().config); },
        nullptr);

    // ---- 7. 服务循环的架子先立起来(accept 线程/信号/看门狗),再开
    // 浏览器——"接口就绪才开浏览器"的接口含 accept 在跑。 ----
    struct ServeState {
        std::mutex queue_mutex;
        std::condition_variable cv;
        std::deque<std::unique_ptr<app_server::WsTransport::Session>> queue;
        std::mutex current_mutex;  // 守 current(接管/收口要跨线程 Close)
        app_server::WsTransport::Session* current = nullptr;
        bool done = false;
    };
    ServeState serve_state;

    std::thread accept_thread([&] {
        web_server.Run([&serve_state](std::unique_ptr<app_server::WsTransport::Session> session,
                                      const app_server::ws::HttpRequestHead& head) {
            // 接管旗(?takeover=1):显式接管只换控制连接,不停后台任务
            //(§六);旧连接被 Close,在跑回合照旧(Detached)。
            const bool takeover = app_server::ws::QueryParam(head.query, "takeover") == "1";
            {
                std::lock_guard<std::mutex> lock(serve_state.current_mutex);
                if (serve_state.current != nullptr && !takeover) {
                    // 占用通报:一帧话 + close。页面显示"被占用,可接管"。
                    const nlohmann::json notice = nlohmann::json{
                        {"method", "assistant/connection/occupied"},
                        {"params", {{"hint", "takeover"}, {"detail", "已有页面持有控制连接"}}}};
                    session->SendMessage(notice.dump());
                    session->Close();
                    return;
                }
                if (serve_state.current != nullptr && takeover) {
                    serve_state.current->Close();  // 线程安全;服务线程随后收线换班
                }
            }
            {
                std::lock_guard<std::mutex> lock(serve_state.queue_mutex);
                serve_state.queue.push_back(std::move(session));
            }
            serve_state.cv.notify_one();
        });
        {
            std::lock_guard<std::mutex> lock(serve_state.queue_mutex);
            serve_state.done = true;
        }
        serve_state.cv.notify_all();
    });

    // Ctrl+C 收口:置旗后由看门狗停监听 + 断当前连接,主循环自然退出。
    std::signal(SIGINT, HandleAssistantSignal);
    std::signal(SIGTERM, HandleAssistantSignal);
    std::thread watchdog([&] {
        while (!AssistantStopFlag().load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        web_server.Stop();
        {
            std::lock_guard<std::mutex> lock(serve_state.current_mutex);
            if (serve_state.current != nullptr) {
                serve_state.current->Close();
            }
        }
    });

    // ---- 8. URL 就绪→开浏览器(监听/accept/健康都已就绪;打开失败
    // URL 已打印可复制) ----
    const std::string url = web_auth->MakeBootstrapUrl(web_server.actual_port());
    std::fprintf(stderr, "LubanCode 助理已就绪(profile=%s, 127.0.0.1:%d)\n", profile.c_str(),
                 web_server.actual_port());
    std::fprintf(stderr, "  打开地址: %s\n", url.c_str());
    std::fprintf(stderr,
                 "  (浏览器打不开就把上面整行复制到地址栏;关掉本窗口/终端,助理随进程停止"
                 "——关网页不影响在跑任务)\n");
    if (!args.no_open) {
        OpenBrowserFor(url);
    }

    int exit_code = 0;
    // ---- 9. 主循环:串行服务控制连接(单活跃控制连接,§六) ----
    while (true) {
        std::unique_ptr<app_server::WsTransport::Session> session;
        {
            std::unique_lock<std::mutex> lock(serve_state.queue_mutex);
            serve_state.cv.wait(lock, [&serve_state] { return serve_state.done || !serve_state.queue.empty(); });
            if (serve_state.queue.empty()) {
                break;  // 收摊:监听停了且没有后续连接
            }
            session = std::move(serve_state.queue.front());
            serve_state.queue.pop_front();
        }
        app_server::Server::WsServeOutcome outcome =
            app_server::Server::WsServeOutcome::Disconnected;
        {
            std::lock_guard<std::mutex> lock(serve_state.current_mutex);
            serve_state.current = session.get();
        }
        outcome = server.ServeWsSessionBorrowed(session);
        {
            std::lock_guard<std::mutex> lock(serve_state.current_mutex);
            serve_state.current = nullptr;
        }
        session.reset();  // 连接收线(Session 析构=close 尽力)
        if (outcome == app_server::Server::WsServeOutcome::ExitRequested) {
            std::fprintf(stderr, "[assistant] 收到 shutdown/exit,停止助理(在跑任务按打断收口)\n");
            break;
        }
        if (AssistantStopFlag().load()) {
            break;
        }
    }

    // ---- 9. 宽限收口:停收新活→收口→释放监听与锁(§四) ----
    web_server.Stop();
    accept_thread.join();
    AssistantStopFlag().store(true);
    if (watchdog.joinable()) {
        watchdog.join();
    }
    server.Shutdown();
    lock.Release();
    std::fprintf(stderr, "[assistant] 已收口(profile=%s)\n", profile.c_str());
    return exit_code;
}

}  // namespace lubancode::app
