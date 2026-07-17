// 配置四级合并:专属 env(1 级)> 配置文件(2 级)> 通用 env(3 级)>
// 内置默认值(4 级),按字段逐个决,不是整套配置一刀切。
// 全部用纯函数测(MergeConfig / ParseFileConfigJson / RequireApiKey),
// 不真读环境变量、不真读磁盘文件。

#include <doctest/doctest.h>

#include <string>

#include "config/config.hpp"

using namespace lubancode;

namespace {

config::LubancodeEnvValues EmptyLubancodeEnv() {
    return config::LubancodeEnvValues{};
}

config::GenericEnvValues EmptyGenericEnv() {
    return config::GenericEnvValues{};
}

}  // namespace

// ---------------------------------------------------------------------------
// 四级优先级:每一级都设置同一字段,验证高优先级压过低优先级。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: 什么都没设置时,wire/max_context_chars 走内置默认值,base_url/model/api_key 留空") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());

    // lubancode 不绑死哪一家模型服务:base_url、model、api_key 都没有内置默认值。
    CHECK(result->config.wire == config::Wire::Anthropic);
    CHECK(result->config.base_url.empty());
    CHECK(result->config.model.empty());
    CHECK(result->config.auth_token.empty());
    CHECK(result->config.max_context_chars == config::kDefaultMaxContextChars);

    CHECK(result->sources.wire == config::Source::Default);
    CHECK(result->sources.base_url == config::Source::Default);
    CHECK(result->sources.model == config::Source::Default);
    CHECK(result->sources.auth_token == config::Source::Default);
    CHECK(result->sources.max_context_chars == config::Source::Default);
}

TEST_CASE("MergeConfig: 通用环境变量压过内置默认值") {
    config::GenericEnvValues generic;
    generic.anthropic_base_url = "https://generic.example.com";
    generic.anthropic_auth_token = "generic-token";
    generic.anthropic_model = "generic-model";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, generic);
    REQUIRE(result.has_value());

    CHECK(result->config.base_url == "https://generic.example.com");
    CHECK(result->config.auth_token == "generic-token");
    CHECK(result->config.model == "generic-model");

    CHECK(result->sources.base_url == config::Source::GenericEnv);
    CHECK(result->sources.auth_token == config::Source::GenericEnv);
    CHECK(result->sources.model == config::Source::GenericEnv);
}

TEST_CASE("MergeConfig: 配置文件压过通用环境变量") {
    config::GenericEnvValues generic;
    generic.anthropic_base_url = "https://generic.example.com";
    generic.anthropic_auth_token = "generic-token";
    generic.anthropic_model = "generic-model";

    config::FileConfig file;
    file.base_url = "https://file.example.com";
    file.api_key = "file-key";
    file.model = "file-model";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, generic);
    REQUIRE(result.has_value());

    CHECK(result->config.base_url == "https://file.example.com");
    CHECK(result->config.auth_token == "file-key");
    CHECK(result->config.model == "file-model");

    CHECK(result->sources.base_url == config::Source::ConfigFile);
    CHECK(result->sources.auth_token == config::Source::ConfigFile);
    CHECK(result->sources.model == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.base_url = "https://lubancode.example.com";
    lubancode_env.api_key = "lubancode-key";
    lubancode_env.model = "lubancode-model";

    config::FileConfig file;
    file.base_url = "https://file.example.com";
    file.api_key = "file-key";
    file.model = "file-model";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());

    CHECK(result->config.base_url == "https://lubancode.example.com");
    CHECK(result->config.auth_token == "lubancode-key");
    CHECK(result->config.model == "lubancode-model");

    CHECK(result->sources.base_url == config::Source::LubancodeEnv);
    CHECK(result->sources.auth_token == config::Source::LubancodeEnv);
    CHECK(result->sources.model == config::Source::LubancodeEnv);
}

TEST_CASE("MergeConfig: 四级全设置时,专属 env 全面胜出") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.base_url = "https://lubancode.example.com";
    lubancode_env.api_key = "lubancode-key";
    lubancode_env.model = "lubancode-model";
    lubancode_env.max_context_chars = 12345;

    config::FileConfig file;
    file.base_url = "https://file.example.com";
    file.api_key = "file-key";
    file.model = "file-model";
    file.max_context_chars = 999;
    file.source_path = "/tmp/.lubancode.json";

    config::GenericEnvValues generic;
    generic.anthropic_base_url = "https://generic.example.com";
    generic.anthropic_auth_token = "generic-token";
    generic.anthropic_model = "generic-model";

    const auto result = config::MergeConfig(lubancode_env, file, generic);
    REQUIRE(result.has_value());

    CHECK(result->config.base_url == "https://lubancode.example.com");
    CHECK(result->config.auth_token == "lubancode-key");
    CHECK(result->config.model == "lubancode-model");
    CHECK(result->config.max_context_chars == 12345);
}

// ---------------------------------------------------------------------------
// 配置文件部分字段缺失:没写的字段该去哪一级找哪一级。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: 配置文件只写了 base_url 和 api_key,model 从通用 env 来") {
    config::GenericEnvValues generic;
    generic.anthropic_model = "generic-model";
    generic.anthropic_base_url = "https://should-not-be-used.example.com";  // 文件优先,不该用到这个

    config::FileConfig file;
    file.base_url = "https://file.example.com";
    file.api_key = "file-key";
    // model、max_context_chars 没写
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, generic);
    REQUIRE(result.has_value());

    CHECK(result->config.base_url == "https://file.example.com");
    CHECK(result->sources.base_url == config::Source::ConfigFile);

    CHECK(result->config.auth_token == "file-key");
    CHECK(result->sources.auth_token == config::Source::ConfigFile);

    CHECK(result->config.model == "generic-model");
    CHECK(result->sources.model == config::Source::GenericEnv);

    CHECK(result->config.max_context_chars == config::kDefaultMaxContextChars);
    CHECK(result->sources.max_context_chars == config::Source::Default);
}

TEST_CASE("MergeConfig: 配置文件只写了 model,base_url/api_key 没有默认值,留空") {
    config::FileConfig file;
    file.model = "only-model-from-file";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());

    CHECK(result->config.model == "only-model-from-file");
    CHECK(result->sources.model == config::Source::ConfigFile);

    CHECK(result->config.base_url.empty());
    CHECK(result->sources.base_url == config::Source::Default);

    CHECK(result->config.auth_token.empty());
    CHECK(result->sources.auth_token == config::Source::Default);
}

// ---------------------------------------------------------------------------
// wire 决定用哪一组通用环境变量、哪一套默认值。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: wire=responses 时,通用 env 读 OPENAI_*,不读 ANTHROPIC_*") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.wire = "responses";

    config::GenericEnvValues generic;
    generic.anthropic_base_url = "https://anthropic-should-not-be-used.example.com";
    generic.anthropic_auth_token = "anthropic-should-not-be-used";
    generic.openai_base_url = "https://openai.example.com";
    generic.openai_api_key = "openai-key";
    generic.openai_model = "openai-model";

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, generic);
    REQUIRE(result.has_value());

    CHECK(result->config.wire == config::Wire::Responses);
    CHECK(result->config.base_url == "https://openai.example.com");
    CHECK(result->config.auth_token == "openai-key");
    CHECK(result->config.model == "openai-model");
}

TEST_CASE("MergeConfig: wire=responses 且什么都没配时,base_url/model 一样没有默认值,留空") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.wire = "responses";

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());

    CHECK(result->config.base_url.empty());
    CHECK(result->config.model.empty());
}

TEST_CASE("MergeConfig: 配置文件里的 wire 压过默认值,专属 env 的 wire 又压过配置文件") {
    config::FileConfig file;
    file.wire = "responses";
    file.source_path = "/tmp/.lubancode.json";

    const auto file_only = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(file_only.has_value());
    CHECK(file_only->config.wire == config::Wire::Responses);
    CHECK(file_only->sources.wire == config::Source::ConfigFile);

    config::LubancodeEnvValues lubancode_env;
    lubancode_env.wire = "anthropic";
    const auto env_wins = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(env_wins.has_value());
    CHECK(env_wins->config.wire == config::Wire::Anthropic);
    CHECK(env_wins->sources.wire == config::Source::LubancodeEnv);
}

TEST_CASE("MergeConfig: wire 是不认得的值时报错,错误信息里带上是哪里写的") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.wire = "not-a-real-wire";

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("LUBANCODE_WIRE") != std::string::npos);
    CHECK(result.error().find("not-a-real-wire") != std::string::npos);
}

TEST_CASE("MergeConfig: 配置文件里的 wire 是不认得的值,错误信息带上文件路径") {
    config::FileConfig file;
    file.wire = "bogus";
    file.source_path = "/home/user/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("/home/user/.lubancode.json") != std::string::npos);
    CHECK(result.error().find("bogus") != std::string::npos);
}

// ---------------------------------------------------------------------------
// api_key:MergeConfig 本身不报错(留空,来源记 Default),校验交给
// RequireApiKey,好让 --config 在没配 api_key 时也能把其它字段打印出来。
// ---------------------------------------------------------------------------

TEST_CASE("RequireApiKey: api_key 有值时通过") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.api_key = "some-key";
    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(config::RequireApiKey(*result).has_value());
}

TEST_CASE("RequireApiKey: 四级都没有 api_key 时报错,错误信息提到四级来源") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.auth_token.empty());

    const auto check = config::RequireApiKey(*result);
    REQUIRE_FALSE(check.has_value());
    const std::string& message = check.error();
    CHECK(message.find("LUBANCODE_API_KEY") != std::string::npos);
    CHECK(message.find(".lubancode.json") != std::string::npos);
    CHECK(message.find("api_key") != std::string::npos);
    CHECK(message.find("ANTHROPIC_AUTH_TOKEN") != std::string::npos);
    CHECK(message.find("内置默认值") != std::string::npos);
}

TEST_CASE("RequireApiKey: wire=responses 时,错误信息提到 OPENAI_API_KEY 而不是 ANTHROPIC_AUTH_TOKEN") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.wire = "responses";
    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());

    const auto check = config::RequireApiKey(*result);
    REQUIRE_FALSE(check.has_value());
    CHECK(check.error().find("OPENAI_API_KEY") != std::string::npos);
    CHECK(check.error().find("ANTHROPIC_AUTH_TOKEN") == std::string::npos);
}

// ---------------------------------------------------------------------------
// max_context_chars:只有专属 env / 配置文件 / 默认值三级,没有通用 env 这一级。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: max_context_chars 配置文件压过默认值") {
    config::FileConfig file;
    file.max_context_chars = 777;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_context_chars == 777);
    CHECK(result->sources.max_context_chars == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: max_context_chars 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.max_context_chars = 555;

    config::FileConfig file;
    file.max_context_chars = 777;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_context_chars == 555);
    CHECK(result->sources.max_context_chars == config::Source::LubancodeEnv);
}

// ---------------------------------------------------------------------------
// ParseFileConfigJson:纯函数,给定 JSON 文本直接测,不碰磁盘。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: 完整字段都能解出来") {
    const std::string json = R"({
        "wire": "anthropic",
        "base_url": "https://api.minimaxi.com/anthropic",
        "api_key": "sk-xxx",
        "model": "MiniMax-M3",
        "max_context_chars": 600000
    })";

    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    REQUIRE(result->wire.has_value());
    CHECK(*result->wire == "anthropic");
    REQUIRE(result->base_url.has_value());
    CHECK(*result->base_url == "https://api.minimaxi.com/anthropic");
    REQUIRE(result->api_key.has_value());
    CHECK(*result->api_key == "sk-xxx");
    REQUIRE(result->model.has_value());
    CHECK(*result->model == "MiniMax-M3");
    REQUIRE(result->max_context_chars.has_value());
    CHECK(*result->max_context_chars == 600000);
}

TEST_CASE("ParseFileConfigJson: 字段全部可选,缺的留 nullopt") {
    const std::string json = R"({"model": "只写了这一个"})";

    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    CHECK_FALSE(result->wire.has_value());
    CHECK_FALSE(result->base_url.has_value());
    CHECK_FALSE(result->api_key.has_value());
    REQUIRE(result->model.has_value());
    CHECK(*result->model == "只写了这一个");
    CHECK_FALSE(result->max_context_chars.has_value());
}

TEST_CASE("ParseFileConfigJson: 空 object 全部字段都是 nullopt") {
    const auto result = config::ParseFileConfigJson("{}", "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    CHECK_FALSE(result->wire.has_value());
    CHECK_FALSE(result->base_url.has_value());
    CHECK_FALSE(result->api_key.has_value());
    CHECK_FALSE(result->model.has_value());
    CHECK_FALSE(result->max_context_chars.has_value());
}

TEST_CASE("ParseFileConfigJson: 坏 JSON 报错,错误信息带上文件路径") {
    const std::string bad_json = "{ this is not valid json, }";

    const auto result = config::ParseFileConfigJson(bad_json, "/home/user/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("/home/user/.lubancode.json") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: 顶层不是 object(比如是数组)也报错,带路径") {
    const auto result = config::ParseFileConfigJson("[1, 2, 3]", "/home/user/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("/home/user/.lubancode.json") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: 字段类型不对(比如 base_url 写成数字)报错") {
    const std::string json = R"({"base_url": 12345})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("base_url") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: max_context_chars 不是正整数时报错") {
    const std::string json = R"({"max_context_chars": -5})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("max_context_chars") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ToString(Source):--config 诊断输出用的中文说法,四种来源都要有说法。
// ---------------------------------------------------------------------------

TEST_CASE("ToString(Source): 四种来源都有非空的中文说法") {
    CHECK_FALSE(config::ToString(config::Source::LubancodeEnv).empty());
    CHECK_FALSE(config::ToString(config::Source::ConfigFile).empty());
    CHECK_FALSE(config::ToString(config::Source::GenericEnv).empty());
    CHECK_FALSE(config::ToString(config::Source::Default).empty());
}

// ---------------------------------------------------------------------------
// RequireConfigured:base_url/api_key/model 三个字段都不许空,非交互路径
// (单发模式/管道模式)用这个,跟只管 api_key 一个字段的 RequireApiKey 分开。
// ---------------------------------------------------------------------------

TEST_CASE("RequireConfigured: 三个字段都有值时通过") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.base_url = "https://example.com";
    lubancode_env.api_key = "some-key";
    lubancode_env.model = "some-model";
    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(config::RequireConfigured(*result).has_value());
}

TEST_CASE("RequireConfigured: 什么都没配时报错,三个字段都点名,并且提到三条配置途径") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());

    const auto check = config::RequireConfigured(*result);
    REQUIRE_FALSE(check.has_value());
    const std::string& message = check.error();
    CHECK(message.find("base_url") != std::string::npos);
    CHECK(message.find("api_key") != std::string::npos);
    CHECK(message.find("model") != std::string::npos);
    CHECK(message.find("向导") != std::string::npos);
    CHECK(message.find(".lubancode.json") != std::string::npos);
    CHECK(message.find("LUBANCODE_") != std::string::npos);
}

TEST_CASE("RequireConfigured: 只缺 model 时,错误信息只点名 model,不提 base_url/api_key") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.base_url = "https://example.com";
    lubancode_env.api_key = "some-key";
    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());

    const auto check = config::RequireConfigured(*result);
    REQUIRE_FALSE(check.has_value());
    const std::string& message = check.error();
    CHECK(message.find("model") != std::string::npos);
    CHECK(message.find("base_url") == std::string::npos);
    CHECK(message.find("api_key") == std::string::npos);
}

// ---------------------------------------------------------------------------
// MaskApiKey:打码规则。
// ---------------------------------------------------------------------------

TEST_CASE("MaskApiKey: 空字符串显示未设置") {
    CHECK(config::MaskApiKey("") == "(未设置)");
}

TEST_CASE("MaskApiKey: 超过 8 位只留前 8 位加省略号") {
    CHECK(config::MaskApiKey("sk-cp-abcdefghijklmnop") == "sk-cp-ab...");
}

TEST_CASE("MaskApiKey: 不超过 8 位原样加省略号,不截断出空字符串") {
    CHECK(config::MaskApiKey("short") == "short...");
}

// ---------------------------------------------------------------------------
// ConfigResult::config_file_path:LoadFromEnv 才会填,MergeConfig 是纯函数,
// 不碰路径,默认应该是 std::nullopt。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: 不设置 config_file_path,默认是 std::nullopt") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK_FALSE(result->config_file_path.has_value());
}
