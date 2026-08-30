// SecretResolver 的实现:SecretValue 的 RAII、窄 dotenv parser、env
// provider、数据目录规矩与打码器。不碰网络、不碰 Lua——那两头各有各的
// 合同(阶段 2/3)。
#include "runtime/secret_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>

#include "config/config.hpp"        // HomeLubancodeDir
#include "platform/paths.hpp"       // GetEnvVar/Utf8ToPath/PathToUtf8
#include "platform/text_encoding.hpp"  // IsValidUtf8

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// SecretValue
// ---------------------------------------------------------------------------

void BestEffortZeroizeString(std::string& text) {
    if (text.empty()) {
        return;
    }
    // volatile 指针逐字节清零:防优化器把"写后即毁"的商店整个抹掉。这不是
    // 绝对保密证明(§7.4),是合同上的尽力而为。
    volatile char* p = text.data();
    for (std::size_t i = 0; i < text.size(); ++i) {
        p[i] = '\0';
    }
}

SecretValue::SecretValue(std::string&& value) : data_(std::move(value)) {}

SecretValue::~SecretValue() { BestEffortZeroizeString(data_); }

SecretValue::SecretValue(SecretValue&& other) noexcept : data_(std::move(other.data_)) {
    // 源对象的 buffer 还在(容量未释放),把残留字节清掉,不留第二份。
    BestEffortZeroizeString(other.data_);
}

SecretValue& SecretValue::operator=(SecretValue&& other) noexcept {
    if (this != &other) {
        BestEffortZeroizeString(data_);
        data_ = std::move(other.data_);
        BestEffortZeroizeString(other.data_);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// 分型与状态账
// ---------------------------------------------------------------------------

std::string_view SecretResolveIssueName(SecretResolveIssue issue) {
    switch (issue) {
        case SecretResolveIssue::NotDeclared:
            return "secret_not_declared";
        case SecretResolveIssue::Missing:
            return "secret_missing";
        case SecretResolveIssue::DotenvInvalid:
            return "dotenv_invalid";
    }
    return "secret_missing";
}

std::string_view SecretSourceName(SecretSource source) {
    switch (source) {
        case SecretSource::None:
            return "missing";
        case SecretSource::HostEnv:
            return "host env";
        case SecretSource::PluginDotEnv:
            return "user dotenv";
    }
    return "missing";
}

std::string SecretStatus::Format() const {
    std::string out = id;
    out += " <- ";
    out += env;
    out += required ? " (required, " : " (optional, ";
    out += available ? "available via " : "missing";
    if (available) {
        out += SecretSourceName(source);
    }
    out += ")";
    if (!diagnostic.empty()) {
        out += " [";
        out += diagnostic;
        out += "]";
    }
    return out;
}

// ---------------------------------------------------------------------------
// 窄 dotenv parser(§7.3)
// ---------------------------------------------------------------------------

namespace {

// key 规矩:[A-Za-z_][A-Za-z0-9_]*。
bool DotenvKeyAllowed(std::string_view key) {
    if (key.empty() || key.size() > 128) {
        return false;
    }
    const char first = key.front();
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::string_view TrimSpaces(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// dotenv 子集的最大体积:过 1 MiB 直接拒读(§7.3)。
inline constexpr std::uintmax_t kDotenvMaxBytes = 1024 * 1024;

}  // namespace

std::expected<std::map<std::string, std::string>, std::string> ParseDotenvText(
    std::string_view text, const std::set<std::string>& allowed_keys, std::vector<std::string>& warnings) {
    std::string_view body = text;
    // UTF-8 BOM 剥掉;别的 BOM(UTF-16/32)不是 UTF-8,拒。
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        body.remove_prefix(3);
    }
    if (!platform::IsValidUtf8(std::string(body))) {
        return std::unexpected("dotenv 不是合法 UTF-8");
    }

    std::map<std::string, std::string> values;
    int line_number = 0;
    std::size_t start = 0;
    while (start <= body.size()) {
        std::size_t end = body.find('\n', start);
        std::string_view line = body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++line_number;
        const std::string_view trimmed = TrimSpaces(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            // 空行与注释略过。
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
            continue;
        }
        // export 不做:行首 export 即拒,不猜是不是值的一部分。
        if (trimmed.size() >= 6 && trimmed.substr(0, 6) == "export" &&
            (trimmed.size() == 6 || trimmed[6] == ' ' || trimmed[6] == '\t')) {
            return std::unexpected("dotenv 第 " + std::to_string(line_number) +
                                   " 行不收 export(密钥文件不是 shell 脚本)");
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected("dotenv 第 " + std::to_string(line_number) + " 行没有 =(只收 KEY=value)");
        }
        const std::string_view key = TrimSpaces(trimmed.substr(0, eq));
        if (!DotenvKeyAllowed(key)) {
            return std::unexpected("dotenv 第 " + std::to_string(line_number) + " 行的 key 不合规矩(只收 "
                                                                                   "[A-Za-z_][A-Za-z0-9_]*)");
        }
        std::string_view raw_value = TrimSpaces(trimmed.substr(eq + 1));
        // 引号包整值:单双引号都收;引号内不许换行(多行拒)、未闭合拒。
        // 裸值与双引号值里的 '$' 按插值嫌疑拒;单引号值里 '$' 是字面(shell
        // 同款语义)。反引号(命令替换)任何值里都拒。
        bool single_quoted = false;
        if (raw_value.size() >= 2 && raw_value.front() == '"' && raw_value.back() == '"') {
            raw_value = raw_value.substr(1, raw_value.size() - 2);
        } else if (raw_value.size() >= 2 && raw_value.front() == '\'' && raw_value.back() == '\'') {
            raw_value = raw_value.substr(1, raw_value.size() - 2);
            single_quoted = true;
        } else if ((raw_value.size() >= 1 && raw_value.front() == '"') ||
                   (raw_value.size() >= 1 && raw_value.front() == '\'')) {
            return std::unexpected("dotenv 第 " + std::to_string(line_number) +
                                   " 行的引号没闭合(不做多行值)");
        }
        if (raw_value.find('`') != std::string_view::npos) {
            return std::unexpected("dotenv 第 " + std::to_string(line_number) + " 行不收反引号(命令替换不做)");
        }
        if (!single_quoted && raw_value.find('$') != std::string_view::npos) {
            return std::unexpected("dotenv 第 " + std::to_string(line_number) +
                                   " 行不收 $(不做变量插值;单引号里的 $ 才是字面)");
        }
        // 只装声明过的键:别的行语法已过(坏了整份拒),但值不进结果表。
        if (allowed_keys.count(std::string(key)) != 0) {
            const auto [existing, inserted] = values.emplace(std::string(key), std::string(raw_value));
            if (!inserted) {
                // 同名后写覆盖前写:记一条不带值的 warning。
                warnings.push_back("dotenv 第 " + std::to_string(line_number) + " 行重复定义 " + std::string(key) +
                                   ",后写覆盖前写");
                existing->second = std::string(raw_value);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

std::expected<std::map<std::string, std::string>, std::string> ParseDotenvFile(
    const std::filesystem::path& path, const std::set<std::string>& allowed_keys,
    std::vector<std::string>& warnings) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        return std::unexpected("dotenv 不存在或不是普通文件: " + platform::PathToUtf8(path));
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected("dotenv 大小读不出: " + platform::PathToUtf8(path));
    }
    if (size > kDotenvMaxBytes) {
        return std::unexpected("dotenv 超过 1 MiB 直接拒读(" + std::to_string(size) + " 字节)");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected("dotenv 打不开: " + platform::PathToUtf8(path));
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    file.read(content.data(), static_cast<std::streamsize>(size));
    const std::size_t got = static_cast<std::size_t>(file.gcount());
    content.resize(got);
    return ParseDotenvText(content, allowed_keys, warnings);
}

// ---------------------------------------------------------------------------
// 数据目录规矩(§7.2)
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path> StandalonePluginDataDir(std::string_view plugin_id) {
    const auto home = config::HomeLubancodeDir();
    if (!home.has_value() || plugin_id.empty()) {
        return std::nullopt;
    }
    return platform::Utf8ToPath(*home) / "plugin-data" /
           platform::Utf8ToPath(std::string(plugin_id));
}

std::optional<std::filesystem::path> PackagedPluginDataDir(std::string_view package_id,
                                                           std::string_view local_id) {
    const auto home = config::HomeLubancodeDir();
    if (!home.has_value() || package_id.empty() || local_id.empty()) {
        return std::nullopt;
    }
    return platform::Utf8ToPath(*home) / "package-data" /
           platform::Utf8ToPath(std::string(package_id)) / "plugins" /
           platform::Utf8ToPath(std::string(local_id));
}

// ---------------------------------------------------------------------------
// EnvDotEnvSecretResolver
// ---------------------------------------------------------------------------

namespace {

std::optional<std::string> DefaultEnvLookup(const std::string& name) {
    return platform::GetEnvVar(name.c_str());
}

}  // namespace

EnvDotEnvSecretResolver::EnvDotEnvSecretResolver(SecretResolverOptions options)
    : options_(std::move(options)) {}

std::optional<std::string> EnvDotEnvSecretResolver::ReadDotenvValue(const std::string& env_name) const {
    if (!options_.plugin_data_dir.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path env_path = *options_.plugin_data_dir / ".env";
    // 文件不在是常态(匿名模式),不算坏账,直接按缺失收。
    std::error_code ec;
    if (!std::filesystem::is_regular_file(env_path, ec) || ec) {
        return std::nullopt;
    }
    // 只装声明键:allowed_keys 只放当前这一枚。文件小(≤1 MiB 帽),每次
    // 现读(§10.2:轮换 Key 无需重启),resolver 不缓存明文。
    std::vector<std::string> warnings;  // 重复键的账不带值,给诊断用
    auto parsed = ParseDotenvFile(env_path, std::set<std::string>{env_name}, warnings);
    if (!parsed.has_value()) {
        // 在而读不动/超限/语法坏:记 doctor 诊断,坏账不当值用。
        dotenv_healthy_ = false;
        if (dotenv_diagnostic_.empty()) {
            dotenv_diagnostic_ = parsed.error();
        }
        return std::nullopt;
    }
    const auto it = parsed->find(env_name);
    if (it == parsed->end() || it->second.empty()) {
        return std::nullopt;
    }
    return std::move(it->second);
}

std::expected<SecretValue, SecretResolveError> EnvDotEnvSecretResolver::Resolve(
    const SecretDeclaration& declaration) {
    // 只认构造时声明的 env 名(§5.1:Lua 不可另传名字;resolver 侧的同一
    // 道闩——没进声明表的名字一律 NotDeclared,环境里有没有都不看)。
    bool declared = false;
    for (const SecretDeclaration& known : options_.declarations) {
        if (known.env == declaration.env && known.id == declaration.id) {
            declared = true;
            break;
        }
    }
    if (!declared) {
        SecretResolveError error;
        error.issue = SecretResolveIssue::NotDeclared;
        error.message = "Secret " + declaration.id + " 未在 manifest 声明";
        return std::unexpected(error);
    }
    SecretResolveError error;
    error.issue = SecretResolveIssue::Missing;
    error.message = "Secret " + declaration.id + " (" + declaration.env + ") 未找到";
    // 第一来源:宿主进程环境(§7.1;GetEnvVar 空串即没有)。
    const auto& lookup = options_.env_lookup ? options_.env_lookup : SecretEnvLookup(&DefaultEnvLookup);
    if (auto from_env = lookup(declaration.env); from_env.has_value() && !from_env->empty()) {
        return SecretValue(std::move(*from_env));
    }
    // 第二来源:插件数据目录的 .env(现读,只装这枚声明键)。
    if (auto from_dotenv = ReadDotenvValue(declaration.env); from_dotenv.has_value()) {
        return SecretValue(std::move(*from_dotenv));
    }
    if (declaration.required) {
        return std::unexpected(error);
    }
    // optional 且未找到:合法的空值,调用方降级匿名。
    return SecretValue(std::string());
}

SecretStatus EnvDotEnvSecretResolver::Describe(const SecretDeclaration& declaration) {
    SecretStatus status;
    status.id = declaration.id;
    status.env = declaration.env;
    status.required = declaration.required;
    const auto& lookup = options_.env_lookup ? options_.env_lookup : SecretEnvLookup(&DefaultEnvLookup);
    if (const auto from_env = lookup(declaration.env); from_env.has_value() && !from_env->empty()) {
        status.available = true;
        status.source = SecretSource::HostEnv;
        return status;
    }
    if (auto from_dotenv = ReadDotenvValue(declaration.env); from_dotenv.has_value()) {
        status.available = true;
        status.source = SecretSource::PluginDotEnv;
    }
    if (!dotenv_healthy_ && !dotenv_diagnostic_.empty()) {
        status.diagnostic = dotenv_diagnostic_;
    }
    return status;
}

// ---------------------------------------------------------------------------
// SecretRedactor
// ---------------------------------------------------------------------------

SecretRedactor::~SecretRedactor() {
    for (std::string& secret : secrets_) {
        BestEffortZeroizeString(secret);
    }
}

SecretRedactor::SecretRedactor(SecretRedactor&& other) noexcept : secrets_(std::move(other.secrets_)) {}

SecretRedactor& SecretRedactor::operator=(SecretRedactor&& other) noexcept {
    if (this != &other) {
        for (std::string& secret : secrets_) {
            BestEffortZeroizeString(secret);
        }
        secrets_ = std::move(other.secrets_);
    }
    return *this;
}

void SecretRedactor::Register(const SecretValue& value) {
    if (!value.HasValue()) {
        return;
    }
    const std::string_view view = value.View();
    // 同值不重复登记。
    for (const std::string& existing : secrets_) {
        if (existing == view) {
            return;
        }
    }
    secrets_.emplace_back(view);
}

std::string SecretRedactor::Redact(std::string_view text) const {
    std::string out(text);
    if (secrets_.empty()) {
        return out;
    }
    // 从长到短替换:免得短的先动手把长的截成碎块漏掉尾巴。
    std::vector<std::string_view> ordered(secrets_.begin(), secrets_.end());
    std::sort(ordered.begin(), ordered.end(),
              [](std::string_view a, std::string_view b) { return a.size() > b.size(); });
    for (const std::string_view secret : ordered) {
        std::size_t pos = 0;
        while ((pos = out.find(secret, pos)) != std::string::npos) {
            out.replace(pos, secret.size(), "[REDACTED]");
            pos += 9;  // 越过刚替换进去的 [REDACTED],防自噬。
        }
    }
    return out;
}

}  // namespace lubancode::runtime
