// SecretResolver:插件级 Secret 解析的冻结合同 + env/dotenv provider
// (Lua 受控 HTTP 与 Secret 宿主能力单·阶段 1)。
//
// 所有权规矩(设计单 §四/§7.4):
//   - Secret 从哪儿找、何时可用,唯一 owner 是 SecretResolver;Lua 只拿
//     不透明引用,看不见原文。
//   - Secret 明文寿命 = 宿主侧 SecretValue RAII:禁复制、可移动、析构
//     best-effort 覆写、无 operator<<。
//   - 错误、日志、trace 只记 secret id、来源种类与 available/missing;
//     打码(SecretRedactor)在落日志与模型结果前扫一遍已解析集合。
//
// 查找顺序(§7.1):宿主进程环境中 manifest 声明的 env 名 -> 插件专属
// .env -> 未找到。第一期不接 OS Keychain;接口留了 provider seam,日后
// 加 Windows Credential Manager / macOS Keychain / Secret Service 时 Lua
// 合同不变。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/plugin_contract.hpp"  // SecretDeclaration

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// SecretValue:一段 Secret 明文的宿主侧持有形态(§7.4)。
//
//   - 禁复制,可移动(移动后源的 buffer 已被覆写,不剩第二份)。
//   - 析构时 best-effort 覆写(volatile 逐字节清零,防优化器抹掉)。
//   - 不实现 operator<<,不提供 c_str();要看的只有宿主内的 HTTP 拼
//     头层,走 View()。
//   - best-effort zeroize 不是绝对保密证明:优化器、TLS 库与系统缓冲
//     仍可能留副本(文档如实写)。
// ---------------------------------------------------------------------------
class SecretValue {
public:
    SecretValue() = default;
    // 显式构造:接过一段明文(调用方的串自此作废,由本对象接管寿命)。
    explicit SecretValue(std::string&& value);

    ~SecretValue();  // best-effort 覆写

    SecretValue(const SecretValue&) = delete;
    SecretValue& operator=(const SecretValue&) = delete;

    SecretValue(SecretValue&& other) noexcept;
    SecretValue& operator=(SecretValue&& other) noexcept;

    bool HasValue() const { return !data_.empty(); }

    // 宿主内只读视图(HTTP 层拼 Authorization 头用)。不落日志、不落
    // trace、不进 Lua——那是调用方的纪律,这里不设第二套执法。
    std::string_view View() const { return data_; }

    // 恒 false:不提供输出流。声明成 deleted 模板,误用即编译错。
    template <typename Char, typename Traits>
    friend std::basic_ostream<Char, Traits>& operator<<(std::basic_ostream<Char, Traits>& os,
                                                        const SecretValue&) = delete;

private:
    std::string data_;
};

// 析构/移动时的 best-effort 覆写(导出给 SecretRedactor 共用)。
void BestEffortZeroizeString(std::string& text);

// ---------------------------------------------------------------------------
// 解析结果分型:resolver 层自己的稳定码(与 §11 的 Lua 错误码同文:
// secret_not_declared / secret_missing;dotenv_invalid 是诊断用,不进
// Lua 合同)。
// ---------------------------------------------------------------------------
enum class SecretResolveIssue {
    NotDeclared = 1,   // secret_not_declared:引用了 manifest 未声明的 Secret
    Missing = 2,       // secret_missing:required Secret 没找到
    DotenvInvalid = 3, // dotenv_invalid:.env 坏(超限/编码/语法),诊断明报
};

std::string_view SecretResolveIssueName(SecretResolveIssue issue);

struct SecretResolveError {
    SecretResolveIssue issue = SecretResolveIssue::Missing;
    std::string message;  // 人话;只含 id/来源/原因,绝不含值
};

// Secret 的来源种类(§7.1 的两级 + 空)。
enum class SecretSource {
    None = 0,      // 未找到
    HostEnv,       // 宿主进程环境变量
    PluginDotEnv,  // 插件数据目录的 .env
};

std::string_view SecretSourceName(SecretSource source);  // "missing"/"host env"/"plugin .env"

// 一枚 Secret 的状态账(inspect/doctor 展示用):只有 id、来源与在不在,
// 没有值、没有长度、没有前缀(§10.3)。
struct SecretStatus {
    std::string id;                     // 逻辑 id
    SecretSource source = SecretSource::None;
    bool available = false;
    std::string env;                    // 声明的 env 名(展示)
    bool required = true;               // 声明的 required
    std::string diagnostic;             // .env 坏等非致命诊断(不带值)

    // 一句人话:"api_key <- ANYSEARCH_API_KEY (optional, available via host env)"
    std::string Format() const;
};

// ---------------------------------------------------------------------------
// SecretResolver 接口:按声明解析(阶段 0 冻结的 seam;测试用 fake,生
// 产用 EnvDotEnvSecretResolver)。
// ---------------------------------------------------------------------------
class SecretResolver {
public:
    virtual ~SecretResolver() = default;

    // 按声明解析一枚 Secret。只在工具调用的动态作用域里被调用(顶层加
    // 载期零解析,§九);required=false 且未找到时返回空值(has_value=
    // true、HasValue()=false),由调用方决定匿名降级;required=true 且未
    // 找到返回 SecretMissing。
    virtual std::expected<SecretValue, SecretResolveError> Resolve(const SecretDeclaration& declaration) = 0;

    // 只查状态不取值(inspect/doctor 用;同样不触发 required 分流)。
    virtual SecretStatus Describe(const SecretDeclaration& declaration) = 0;
};

// 宿主环境变量的取值口(缺省 platform::GetEnvVar;测试注入假账,不碰真
// 环境)。
using SecretEnvLookup = std::function<std::optional<std::string>(const std::string& name)>;

// EnvDotEnvSecretResolver 的材料。
struct SecretResolverOptions {
    // 插件数据目录(standalone 的 ~/.lubancode/plugin-data/<id>,或
    // packaged 的 ~/.lubancode/package-data/<包id>/plugins/<local>);.env
    // 在其下。nullopt = 没有 dotenv 来源(只查宿主环境)。插件源码树里的
    // .env 永远不读——路径由宿主算,调用方不许递插件源码目录进来。
    std::optional<std::filesystem::path> plugin_data_dir;
    // manifest 的 Secret 声明全表:.env 只装这里点名的 env 名(§7.3),
    // 未声明的行不进内存结果。空表 = 只查宿主环境。
    std::vector<SecretDeclaration> declarations;
    SecretEnvLookup env_lookup;  // 空 = platform::GetEnvVar
};

// 生产 provider:宿主环境 -> 插件数据目录 .env(§7.1 顺序)。.env 每次
// 解析现读(§10.2:轮换 Key 无需重启),只装声明键;resolver 不缓存
// Secret 明文。
class EnvDotEnvSecretResolver final : public SecretResolver {
public:
    EnvDotEnvSecretResolver() = default;
    explicit EnvDotEnvSecretResolver(SecretResolverOptions options);

    std::expected<SecretValue, SecretResolveError> Resolve(const SecretDeclaration& declaration) override;
    SecretStatus Describe(const SecretDeclaration& declaration) override;

    // .env 是否健康(读挂了/超限/语法坏)与原因(不带值)——doctor 用。
    // 文件不存在是常态(匿名模式),不算坏账。
    bool dotenv_healthy() const { return dotenv_healthy_; }
    const std::string& dotenv_diagnostic() const { return dotenv_diagnostic_; }

private:
    // 单键读 .env:allowed_keys 只放当前声明的 env 名(只解析声明键)。
    // 返回 nullopt = 文件不在/没这个键/坏账(坏账已记诊断)。
    std::optional<std::string> ReadDotenvValue(const std::string& env_name) const;

    SecretResolverOptions options_;
    mutable bool dotenv_healthy_ = true;
    mutable std::string dotenv_diagnostic_;
};

// ---------------------------------------------------------------------------
// 窄 dotenv parser(§7.3)。只认:
//   # 注释 / 空行 / KEY=value / KEY="整值" / KEY='整值'
// 规矩:
//   - UTF-8 或 UTF-8 BOM;编码坏拒。
//   - key 只收 [A-Za-z_][A-Za-z0-9_]*。
//   - 不做 shell 展开、命令替换、变量插值、多行与 export——出现即整份拒。
//   - 同名后写覆盖前写,记一条不带值的 warning。
//   - 文件过 1 MiB 直接拒读。
//   - 只装 allowed_keys 里点名的键;别的行语法仍要过(坏即拒),但值不进
//     内存结果表。
// ---------------------------------------------------------------------------
std::expected<std::map<std::string, std::string>, std::string> ParseDotenvText(
    std::string_view text, const std::set<std::string>& allowed_keys, std::vector<std::string>& warnings);

// 文件版:读盘 + 大小帽 + 调 ParseDotenvText。
std::expected<std::map<std::string, std::string>, std::string> ParseDotenvFile(
    const std::filesystem::path& path, const std::set<std::string>& allowed_keys,
    std::vector<std::string>& warnings);

// ---------------------------------------------------------------------------
// 数据目录规矩(§7.2):standalone 与 packaged 两类 .env 的落点。路径由
// 宿主算,不交 Lua 猜。源码树 .env 永不进这两条路。
// ---------------------------------------------------------------------------
// standalone:<home>/.lubancode/plugin-data/<plugin-id>/
std::optional<std::filesystem::path> StandalonePluginDataDir(std::string_view plugin_id);
// packaged:<home>/.lubancode/package-data/<package-id>/plugins/<local-id>/
std::optional<std::filesystem::path> PackagedPluginDataDir(std::string_view package_id,
                                                           std::string_view local_id);

// ---------------------------------------------------------------------------
// SecretRedactor:错误/日志拼装时的打码器(§7.4)。登记本次调用已解析的
// Secret(短命持有,析构同样 best-effort 覆写),把文本里出现的原文替换
// 成 [REDACTED]。上游错误体原样回显 Secret 时,落日志与模型结果前必过
// 这一道。
// ---------------------------------------------------------------------------
class SecretRedactor {
public:
    SecretRedactor() = default;
    ~SecretRedactor();

    SecretRedactor(const SecretRedactor&) = delete;
    SecretRedactor& operator=(const SecretRedactor&) = delete;
    SecretRedactor(SecretRedactor&& other) noexcept;
    SecretRedactor& operator=(SecretRedactor&& other) noexcept;

    // 登记一枚已解析的 Secret(从 View() 拷一份短命副本;这份副本归
    // redactor 的 RAII 管,析构覆写)。空值不登记。
    void Register(const SecretValue& value);

    // 替换文本中出现的已登记原文为 [REDACTED](逐枚替换,从长到短,免得
    // 短的把长的截断)。
    std::string Redact(std::string_view text) const;

    bool empty() const { return secrets_.empty(); }
    std::size_t size() const { return secrets_.size(); }

private:
    std::vector<std::string> secrets_;
};

}  // namespace lubancode::runtime
