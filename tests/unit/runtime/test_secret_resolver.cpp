// SecretResolver 与插件数据 .env 的单测(Lua 受控 HTTP 与 Secret 宿主能
// 力单·阶段 1)。
//
// 章法:
//   - 全部假 Key,一律 FAKE_ 前缀明标,不写真钥;
//   - 不碰真环境(env_lookup 注入假账)、不接网;
//   - 泄露扫描回归:解析 -> Describe 文案 -> 错误文案 -> 打码文本,全链
//     路搜不到假 Key 原文(§13.2);
//   - SecretValue 的防泄露证据:禁复制(static_assert)、移动后源覆写、
//     析构覆写、无输出流(不声明 operator<< 即编译错,这里用注释钉)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "runtime/secret_resolver.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

// 禁复制是类型级合同,编译期钉死。
static_assert(!std::is_copy_constructible_v<SecretValue>);
static_assert(!std::is_copy_assignable_v<SecretValue>);
static_assert(std::is_move_constructible_v<SecretValue>);
static_assert(std::is_move_assignable_v<SecretValue>);
// SecretRedactor 同规矩(短命持有明文副本,同样禁复制)。
static_assert(!std::is_copy_constructible_v<SecretRedactor>);
static_assert(std::is_move_constructible_v<SecretRedactor>);
// EnvDotEnvSecretResolver 只在装配方手里传引用,不必可复制;接口多态。

namespace {

// 临时数据目录(每 CASE 各建各的)。
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_secret_test_" + std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

  private:
    static int counter_;
};
int TempDir::counter_ = 0;

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

// 假环境账:递进去什么就查到什么,不碰真环境。
SecretEnvLookup FakeEnv(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const std::string& name) -> std::optional<std::string> {
        const auto it = values.find(name);
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

const SecretDeclaration kKeyDecl{"api_key", "LUBAN_TEST_API_KEY", true};
const SecretDeclaration kOptionalDecl{"api_key", "LUBAN_TEST_API_KEY", false};

}  // namespace

// ---------------------------------------------------------------------------
// SecretValue:RAII 防泄露证据
// ---------------------------------------------------------------------------

TEST_CASE("SecretValue:覆写原语把明文字节清零(best-effort zeroize)") {
    // BestEffortZeroizeString 是 SecretValue 析构/移动共用的覆写原语,直接
    // 钉行为:清完后原 buffer 里搜不到明文的任何字符。
    std::string text = "FAKE_probe_secret";
    BestEffortZeroizeString(text);
    CHECK(text.find('F') == std::string::npos);
    CHECK(text.find('p') == std::string::npos);
    CHECK(text.find_first_not_of('\0') == std::string::npos);  // 全零字节
    // 析构路径不抛、语义正确(View 合同)。
    {
        SecretValue value(std::string("FAKE_probe_secret"));
        REQUIRE(value.HasValue());
        REQUIRE(value.View() == "FAKE_probe_secret");
    }
    CHECK(true);
}

TEST_CASE("SecretValue:移动后源不再持有明文") {
    SecretValue first(std::string("FAKE_moved_secret"));
    SecretValue second(std::move(first));
    REQUIRE(second.HasValue());
    CHECK(second.View() == "FAKE_moved_secret");
    // 源已被覆写:既不 HasValue,View 也读不到旧文。
    CHECK_FALSE(first.HasValue());
    CHECK(first.View().empty());
    // 移动赋值同规矩。
    SecretValue third;
    third = std::move(second);
    REQUIRE(third.HasValue());
    CHECK(third.View() == "FAKE_moved_secret");
    CHECK_FALSE(second.HasValue());
}

TEST_CASE("SecretValue:空值合法(anonymous 降级的形态)") {
    SecretValue empty;
    CHECK_FALSE(empty.HasValue());
    SecretValue from_empty{std::string()};
    CHECK_FALSE(from_empty.HasValue());
}

TEST_CASE("SecretValue:没有 operator<<(编译期不存在,误用即编译错)") {
    // 合同上不提供输出流重载(§7.4)。若有人日后加上,下面这行的注释去掉
    // 应当编不过;现在保持注释形态,由 static_assert 钉类型不可复制——
    // 流输出需要左值/右值拷贝或友元,禁复制 + 不声明重载双保险:
    //   std::ostringstream os;
    //   os << SecretValue(std::string("FAKE_x"));  // 编译错:无匹配 operator<<
    CHECK(true);
}

// ---------------------------------------------------------------------------
// 窄 dotenv parser(§7.3)
// ---------------------------------------------------------------------------

TEST_CASE("dotenv:注释/空行/引号整值/BOM/空值按合同解析") {
    std::vector<std::string> warnings;
    const std::string text =
        "\xEF\xBB\xBF# comment\n"
        "\n"
        "LUBAN_TEST_API_KEY=FAKE_plain_value\n"
        "QUOTED=\"FAKE value with spaces\"\n"
        "SINGLE='FAKE literal $dollar'\n"
        "EMPTY=\n"
        "  SPACED  =  FAKE_trimmed  \n";
    auto values = ParseDotenvText(text, {"LUBAN_TEST_API_KEY", "QUOTED", "SINGLE", "EMPTY", "SPACED"}, warnings);
    REQUIRE(values.has_value());
    CHECK((*values)["LUBAN_TEST_API_KEY"] == "FAKE_plain_value");
    CHECK((*values)["QUOTED"] == "FAKE value with spaces");
    CHECK((*values)["SINGLE"] == "FAKE literal $dollar");  // 单引号里 $ 是字面
    CHECK((*values)["EMPTY"] == "");
    CHECK((*values)["SPACED"] == "FAKE_trimmed");
    CHECK(warnings.empty());
}

TEST_CASE("dotenv:只装声明过的键,未声明的行语法仍把关但不进结果") {
    std::vector<std::string> warnings;
    auto values = ParseDotenvText("DECLARED=FAKE_yes\nUNDECLARED=FAKE_no\nBAD LINE\n", {"DECLARED"}, warnings);
    // BAD LINE 没有 =,整份拒(语法把关覆盖全部行,不只声明键)。
    REQUIRE_FALSE(values.has_value());
    auto ok = ParseDotenvText("DECLARED=FAKE_yes\nUNDECLARED=FAKE_no\n", {"DECLARED"}, warnings);
    REQUIRE(ok.has_value());
    CHECK(ok->size() == 1);
    CHECK((*ok)["DECLARED"] == "FAKE_yes");
    CHECK(ok->find("UNDECLARED") == ok->end());
}

TEST_CASE("dotenv:插值/命令替换/export/多行全拒") {
    std::vector<std::string> warnings;
    CHECK_FALSE(ParseDotenvText("K=${OTHER}\n", {"K"}, warnings).has_value());          // 插值
    CHECK_FALSE(ParseDotenvText("K=$OTHER\n", {"K"}, warnings).has_value());            // 插值
    CHECK_FALSE(ParseDotenvText("K=\"has $var\"\n", {"K"}, warnings).has_value());      // 双引号里的 $ 同拒
    CHECK_FALSE(ParseDotenvText("K=`whoami`\n", {"K"}, warnings).has_value());          // 命令替换
    CHECK_FALSE(ParseDotenvText("export K=FAKE_x\n", {"K"}, warnings).has_value());     // export
    CHECK_FALSE(ParseDotenvText("K=\"unclosed\n", {"K"}, warnings).has_value());        // 引号未闭合(多行)
    // 单引号里的 $ 是字面(§7.3 引号整值)。
    auto literal = ParseDotenvText("K='$OTHER'\n", {"K"}, warnings);
    REQUIRE(literal.has_value());
    CHECK((*literal)["K"] == "$OTHER");
}

TEST_CASE("dotenv:同名后写覆盖前写,记不带值的 warning") {
    std::vector<std::string> warnings;
    auto values = ParseDotenvText("K=FAKE_first\nK=FAKE_second\n", {"K"}, warnings);
    REQUIRE(values.has_value());
    CHECK((*values)["K"] == "FAKE_second");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("K") != std::string::npos);
    CHECK(warnings[0].find("FAKE") == std::string::npos);  // warning 不带值
}

TEST_CASE("dotenv:key 规矩只收 [A-Za-z_][A-Za-z0-9_]*") {
    std::vector<std::string> warnings;
    CHECK_FALSE(ParseDotenvText("1BAD=x\n", {"1BAD"}, warnings).has_value());
    CHECK_FALSE(ParseDotenvText("BAD-KEY=x\n", {"BAD-KEY"}, warnings).has_value());
    CHECK_FALSE(ParseDotenvText("BAD KEY=x\n", {}, warnings).has_value());
    auto ok = ParseDotenvText("_ok1=x\n", {"_ok1"}, warnings);
    REQUIRE(ok.has_value());
}

TEST_CASE("dotenv:非 UTF-8 拒;文件过 1 MiB 拒读") {
    std::vector<std::string> warnings;
    // GBK 字节(不是 UTF-8)。
    const std::string gbk = "K=\xd6\xd0\xce\xc4\n";
    CHECK_FALSE(ParseDotenvText(gbk, {"K"}, warnings).has_value());

    TempDir dir;
    // 1 MiB + 1 字节:拒读。
    std::string big(1024 * 1024 + 1, '#');
    big += "\nK=FAKE_x\n";
    WriteFile(dir.path / ".env", big);
    CHECK_FALSE(ParseDotenvFile(dir.path / ".env", {"K"}, warnings).has_value());
    // 恰好 1 MiB(全注释)合法。
    std::string exact(1024 * 1024, '#');
    WriteFile(dir.path / ".env", exact);
    auto ok = ParseDotenvFile(dir.path / ".env", {"K"}, warnings);
    REQUIRE(ok.has_value());
    CHECK(ok->empty());
}

TEST_CASE("dotenv:文件版大小帽与缺文件") {
    TempDir dir;
    std::vector<std::string> warnings;
    // 缺文件:报不存在。
    auto missing = ParseDotenvFile(dir.path / ".env", {"K"}, warnings);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("不存在") != std::string::npos);
    WriteFile(dir.path / ".env", "K=FAKE_file_value\r\n");
    auto values = ParseDotenvFile(dir.path / ".env", {"K"}, warnings);
    REQUIRE(values.has_value());
    CHECK((*values)["K"] == "FAKE_file_value");  // CRLF 剥 \r
}

// ---------------------------------------------------------------------------
// EnvDotEnvSecretResolver:查找顺序与三态(§7.1)
// ---------------------------------------------------------------------------

TEST_CASE("resolver:宿主环境高于 .env;回落 .env;缺失三态(§13.2)") {
    TempDir dir;
    WriteFile(dir.path / ".env", "LUBAN_TEST_API_KEY=FAKE_dotenv_value\n");

    // 态一:环境有值,优先用。
    {
        SecretResolverOptions options;
        options.plugin_data_dir = dir.path;
        options.declarations = {kKeyDecl};
        options.env_lookup = FakeEnv({{"LUBAN_TEST_API_KEY", "FAKE_env_value"}});
        EnvDotEnvSecretResolver resolver(std::move(options));
        auto value = resolver.Resolve(kKeyDecl);
        REQUIRE(value.has_value());
        REQUIRE(value->HasValue());
        CHECK(value->View() == "FAKE_env_value");
        // Describe 也报 host env 来源。
        const auto status = resolver.Describe(kKeyDecl);
        CHECK(status.available);
        CHECK(status.source == SecretSource::HostEnv);
        CHECK(status.Format().find("host env") != std::string::npos);
    }
    // 态二:环境没有,回落插件 .env。
    {
        SecretResolverOptions options;
        options.plugin_data_dir = dir.path;
        options.declarations = {kKeyDecl};
        options.env_lookup = FakeEnv({});
        EnvDotEnvSecretResolver resolver(std::move(options));
        auto value = resolver.Resolve(kKeyDecl);
        REQUIRE(value.has_value());
        REQUIRE(value->HasValue());
        CHECK(value->View() == "FAKE_dotenv_value");
        const auto status = resolver.Describe(kKeyDecl);
        CHECK(status.available);
        CHECK(status.source == SecretSource::PluginDotEnv);
        CHECK(status.Format().find("user dotenv") != std::string::npos);
    }
    // 态三:都没有——required 报 secret_missing,optional 给空值。
    {
        SecretResolverOptions options;
        options.plugin_data_dir = dir.path;
        options.declarations = {kKeyDecl, kOptionalDecl};
        options.env_lookup = FakeEnv({});
        // 换个数据目录(没有 .env)。
        TempDir empty_dir;
        options.plugin_data_dir = empty_dir.path;
        EnvDotEnvSecretResolver resolver(std::move(options));
        auto required = resolver.Resolve(kKeyDecl);
        REQUIRE_FALSE(required.has_value());
        CHECK(required.error().issue == SecretResolveIssue::Missing);
        CHECK(SecretResolveIssueName(required.error().issue) == "secret_missing");
        CHECK(required.error().message.find("FAKE") == std::string::npos);  // 错误文案不带值
        auto optional = resolver.Resolve(kOptionalDecl);
        REQUIRE(optional.has_value());
        CHECK_FALSE(optional->HasValue());
        const auto status = resolver.Describe(kKeyDecl);
        CHECK_FALSE(status.available);
        CHECK(status.source == SecretSource::None);
        CHECK(status.Format().find("missing") != std::string::npos);
    }
}

TEST_CASE("resolver:坏 .env 记诊断,值按缺失收,doctor 明报") {
    TempDir dir;
    WriteFile(dir.path / ".env", "LUBAN_TEST_API_KEY=${LEAK}\n");
    SecretResolverOptions options;
    options.plugin_data_dir = dir.path;
    options.declarations = {kKeyDecl};
    options.env_lookup = FakeEnv({});
    EnvDotEnvSecretResolver resolver(std::move(options));
    auto value = resolver.Resolve(kKeyDecl);
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().issue == SecretResolveIssue::Missing);
    CHECK_FALSE(resolver.dotenv_healthy());
    CHECK_FALSE(resolver.dotenv_diagnostic().empty());
    // 诊断不带值、不带 .env 原文键值。
    CHECK(resolver.dotenv_diagnostic().find("LEAK") == std::string::npos);
    const auto status = resolver.Describe(kKeyDecl);
    CHECK_FALSE(status.available);
    CHECK_FALSE(status.diagnostic.empty());
}

TEST_CASE("resolver:未声明 Secret 一律拒(Lua 不可另传名字)") {
    SecretResolverOptions options;
    options.declarations = {kKeyDecl};
    options.env_lookup = FakeEnv({{"LUBAN_TEST_OTHER", "FAKE_undeclared_value"}});
    EnvDotEnvSecretResolver resolver(std::move(options));
    const SecretDeclaration undeclared{"other_key", "LUBAN_TEST_OTHER", true};
    auto value = resolver.Resolve(undeclared);
    // 声明表里没有的名字 NotDeclared——环境里有没有值都不看。
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().issue == SecretResolveIssue::NotDeclared);
    CHECK(SecretResolveIssueName(value.error().issue) == "secret_not_declared");
    CHECK(value.error().message.find("FAKE_undeclared_value") == std::string::npos);
}

TEST_CASE("resolver:.env 轮换即时生效(每次现读,不缓存)") {
    TempDir dir;
    WriteFile(dir.path / ".env", "LUBAN_TEST_API_KEY=FAKE_round1\n");
    SecretResolverOptions options;
    options.plugin_data_dir = dir.path;
    options.declarations = {kKeyDecl};
    options.env_lookup = FakeEnv({});
    EnvDotEnvSecretResolver resolver(std::move(options));
    {
        auto v1 = resolver.Resolve(kKeyDecl);
        REQUIRE(v1.has_value());
        CHECK(v1->View() == "FAKE_round1");
    }
    WriteFile(dir.path / ".env", "LUBAN_TEST_API_KEY=FAKE_round2\n");
    auto v2 = resolver.Resolve(kKeyDecl);
    REQUIRE(v2.has_value());
    CHECK(v2->View() == "FAKE_round2");
}

// ---------------------------------------------------------------------------
// 数据路径规矩(§7.2)与源码树隔离
// ---------------------------------------------------------------------------

TEST_CASE("数据目录:standalone 与 packaged 两类路径的形状") {
    const auto standalone = StandalonePluginDataDir("anysearch");
    REQUIRE(standalone.has_value());
    const std::string standalone_text = platform::PathToUtf8(*standalone);
    CHECK(standalone_text.find("plugin-data") != std::string::npos);
    CHECK(standalone_text.find("anysearch") != std::string::npos);
    CHECK(standalone_text.find("package-data") == std::string::npos);
    // 真机形状:根在 <home>/.lubancode 之下(HomeLubancodeDir 的真账)。
    CHECK(standalone_text.find(".lubancode") != std::string::npos);

    const auto packaged = PackagedPluginDataDir("moontide.full-stack", "dom-analyzer");
    REQUIRE(packaged.has_value());
    const std::string packaged_text = platform::PathToUtf8(*packaged);
    CHECK(packaged_text.find("package-data") != std::string::npos);
    CHECK(packaged_text.find("moontide.full-stack") != std::string::npos);
    CHECK(packaged_text.find("plugins") != std::string::npos);
    CHECK(packaged_text.find("dom-analyzer") != std::string::npos);
    CHECK(packaged_text.find(".lubancode") != std::string::npos);

    CHECK_FALSE(StandalonePluginDataDir("").has_value());
    CHECK_FALSE(PackagedPluginDataDir("", "x").has_value());
}

TEST_CASE("真环境链路冒烟:进程环境里放进的假 Key 走默认 lookup 可解析") {
    // 不注入 env_lookup——真走 platform::GetEnvVar(Windows _dupenv_s/POSIX
    // getenv)。假 Key 一律 FAKE_ 前缀,测试收尾清掉。变量名与声明一致
    // (LUBAN_TEST_API_KEY)。
#ifdef _WIN32
    REQUIRE(_putenv_s("LUBAN_TEST_API_KEY", "FAKE_smoke_via_real_env") == 0);
#else
    REQUIRE(setenv("LUBAN_TEST_API_KEY", "FAKE_smoke_via_real_env", 1) == 0);
#endif
    {
        SecretResolverOptions options;
        options.declarations = {kOptionalDecl};
        // 不设 env_lookup:默认走真环境。
        EnvDotEnvSecretResolver resolver(std::move(options));
        auto value = resolver.Resolve(kOptionalDecl);
        REQUIRE(value.has_value());
        REQUIRE(value->HasValue());
        CHECK(value->View() == "FAKE_smoke_via_real_env");
        const auto status = resolver.Describe(kOptionalDecl);
        CHECK(status.available);
        CHECK(status.source == SecretSource::HostEnv);
    }
#ifdef _WIN32
    REQUIRE(_putenv_s("LUBAN_TEST_API_KEY", "") == 0);
#else
    unsetenv("LUBAN_TEST_API_KEY");
#endif
}

TEST_CASE("源码树 .env 永不自动读取(resolver 只认数据目录)") {
    // 模拟插件源码树:目录里有 .env,但 resolver 的数据目录另指他处——
    // 源码树那份必须不被读进任何值。
    TempDir source_tree;
    WriteFile(source_tree.path / "plugin.json", "{}");
    WriteFile(source_tree.path / ".env", "LUBAN_TEST_API_KEY=FAKE_source_tree_leak\n");
    TempDir data_dir;  // 空:没有 .env

    SecretResolverOptions options;
    options.plugin_data_dir = data_dir.path;
    options.declarations = {kKeyDecl};
    options.env_lookup = FakeEnv({});
    EnvDotEnvSecretResolver resolver(std::move(options));
    auto value = resolver.Resolve(kKeyDecl);
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().message.find("FAKE_source_tree_leak") == std::string::npos);

    // 没给数据目录(nullopt)同样只查宿主环境,不碰任何源码目录。
    SecretResolverOptions no_dir;
    no_dir.declarations = {kKeyDecl};
    no_dir.env_lookup = FakeEnv({});
    EnvDotEnvSecretResolver resolver2(std::move(no_dir));
    auto value2 = resolver2.Resolve(kKeyDecl);
    REQUIRE_FALSE(value2.has_value());
}

// ---------------------------------------------------------------------------
// 打码器(§7.4)
// ---------------------------------------------------------------------------

TEST_CASE("SecretRedactor:错误体回显原文时替换 [REDACTED]") {
    SecretRedactor redactor;
    SecretValue first(std::string("FAKE_key_abcdef123456"));
    SecretValue second(std::string("FAKE_key_short"));
    redactor.Register(first);
    redactor.Register(second);
    CHECK(redactor.size() == 2);

    const std::string upstream_error =
        "HTTP 401: invalid api key FAKE_key_abcdef123456 provided (hint: FAKE_key_short is wrong)";
    const std::string redacted = redactor.Redact(upstream_error);
    CHECK(redacted.find("FAKE_key_abcdef123456") == std::string::npos);
    CHECK(redacted.find("FAKE_key_short") == std::string::npos);
    // 从长到短替换:短 Key 不把长 Key 截断成漏网的尾巴。
    CHECK(redacted.find("abcdef123456") == std::string::npos);
    CHECK(redacted.find("[REDACTED]") != std::string::npos);
    // 其余文本原样保留。
    CHECK(redacted.find("HTTP 401") != std::string::npos);

    // 空值不登记;同值不重复。
    SecretRedactor dedup;
    SecretValue empty;
    dedup.Register(empty);
    CHECK(dedup.empty());
    dedup.Register(first);
    dedup.Register(first);
    CHECK(dedup.size() == 1);
}

TEST_CASE("SecretRedactor:无登记时原文直通(Redact 不添乱)") {
    SecretRedactor redactor;
    CHECK(redactor.Redact("plain text FAKE_nothing").find("FAKE_nothing") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 泄露扫描回归(§13.2:解析 -> 展示 -> 日志文案全链路搜不到原文)
// ---------------------------------------------------------------------------

TEST_CASE("泄露扫描:假 Key 全链路(解析/状态文案/错误文案/打码)搜不到原文") {
    const std::string fake_key = "FAKE_sk_leak_canary_7f3a9c";
    TempDir dir;
    WriteFile(dir.path / ".env", "LUBAN_TEST_API_KEY=" + fake_key + "\n");

    SecretResolverOptions options;
    options.plugin_data_dir = dir.path;
    options.declarations = {kKeyDecl, kOptionalDecl};
    options.env_lookup = FakeEnv({});
    EnvDotEnvSecretResolver resolver(std::move(options));

    // 链路一:解析出的值只在 SecretValue 里。
    auto value = resolver.Resolve(kOptionalDecl);
    REQUIRE(value.has_value());
    CHECK(value->View() == fake_key);

    // 链路二:状态/展示文案(inspect/doctor 的形状)只有名字与来源。
    const std::string status_line = resolver.Describe(kOptionalDecl).Format();
    CHECK(status_line.find(fake_key) == std::string::npos);
    CHECK(status_line.find("api_key") != std::string::npos);
    CHECK(status_line.find("LUBAN_TEST_API_KEY") != std::string::npos);

    // 链路三:错误文案不带值。
    SecretResolverOptions missing_options;
    missing_options.declarations = {kKeyDecl};
    missing_options.env_lookup = FakeEnv({});
    TempDir empty;
    missing_options.plugin_data_dir = empty.path;
    EnvDotEnvSecretResolver missing_resolver(std::move(missing_options));
    auto missing = missing_resolver.Resolve(kKeyDecl);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message.find(fake_key) == std::string::npos);

    // 链路四:错误/日志拼装走 redaction——上游回显原文时替换掉。
    SecretRedactor redactor;
    redactor.Register(*value);
    const std::string upstream = "Authorization Bearer " + fake_key + " rejected";
    const std::string redacted = redactor.Redact(upstream);
    CHECK(redacted.find(fake_key) == std::string::npos);
    CHECK(redacted.find("Bearer [REDACTED]") != std::string::npos);
}
