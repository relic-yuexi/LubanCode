// 配置四级合并:专属 env(1 级)> 配置文件(2 级)> 通用 env(3 级)>
// 内置默认值(4 级),按字段逐个决,不是整套配置一刀切。
// 全部用纯函数测(MergeConfig / ParseFileConfigJson / RequireApiKey),
// 不真读环境变量、不真读磁盘文件。

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

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
// ParseContextWindowTokens:M6.6 新增,"256k"/"512k"/"1m"/裸数字 -> token 数,
// k/m 按十进制换算,大小写不敏感,坏值(空串/非数字/0/负数)都报错。
// ---------------------------------------------------------------------------

TEST_CASE("ParseContextWindowTokens: 256k/512k/1m 按十进制换算") {
    const auto k256 = config::ParseContextWindowTokens("256k");
    REQUIRE(k256.has_value());
    CHECK(*k256 == 256000);

    const auto k512 = config::ParseContextWindowTokens("512k");
    REQUIRE(k512.has_value());
    CHECK(*k512 == 512000);

    const auto m1 = config::ParseContextWindowTokens("1m");
    REQUIRE(m1.has_value());
    CHECK(*m1 == 1000000);
}

TEST_CASE("ParseContextWindowTokens: k/m 大小写不敏感") {
    const auto upper_k = config::ParseContextWindowTokens("256K");
    REQUIRE(upper_k.has_value());
    CHECK(*upper_k == 256000);

    const auto upper_m = config::ParseContextWindowTokens("1M");
    REQUIRE(upper_m.has_value());
    CHECK(*upper_m == 1000000);
}

TEST_CASE("ParseContextWindowTokens: 裸数字直接当 token 数") {
    const auto result = config::ParseContextWindowTokens("128000");
    REQUIRE(result.has_value());
    CHECK(*result == 128000);
}

TEST_CASE("ParseContextWindowTokens: 空串报错") {
    const auto result = config::ParseContextWindowTokens("");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseContextWindowTokens: 非数字(比如 abc)报错") {
    const auto result = config::ParseContextWindowTokens("abc");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseContextWindowTokens: 0 报错") {
    const auto result = config::ParseContextWindowTokens("0");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseContextWindowTokens: 负数报错") {
    const auto result = config::ParseContextWindowTokens("-100");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseContextWindowTokens: 只有 k/m 后缀没有数字报错") {
    const auto result = config::ParseContextWindowTokens("k");
    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// context_window_tokens:1 级(LUBANCODE_CONTEXT_WINDOW)> 2 级(配置文件)>
// 4 级默认值(256k),没有通用 env 这一级。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: context_window_tokens 什么都没设置时用内置默认值 256k") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.context_window_tokens == config::kDefaultContextWindowTokens);
    CHECK(result->sources.context_window_tokens == config::Source::Default);
}

TEST_CASE("MergeConfig: context_window_tokens 配置文件压过默认值") {
    config::FileConfig file;
    file.context_window = "512k";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.context_window_tokens == 512000);
    CHECK(result->sources.context_window_tokens == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: context_window_tokens 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.context_window = "1m";

    config::FileConfig file;
    file.context_window = "512k";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.context_window_tokens == 1000000);
    CHECK(result->sources.context_window_tokens == config::Source::LubancodeEnv);
}

TEST_CASE("MergeConfig: context_window 坏值报错,错误信息带上是哪里写的") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.context_window = "abc";

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("LUBANCODE_CONTEXT_WINDOW") != std::string::npos);
}

TEST_CASE("MergeConfig: 配置文件里的 context_window 坏值报错,错误信息带文件路径") {
    config::FileConfig file;
    file.context_window = "0";
    file.source_path = "/home/user/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("/home/user/.lubancode.json") != std::string::npos);
}

// ---------------------------------------------------------------------------
// compact_model:1 级 > 2 级 > 4 级默认值(空串 = 跟当前会话模型一致),
// 没有通用 env 这一级、没有取值校验。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: compact_model 什么都没设置时留空") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.compact_model.empty());
    CHECK(result->sources.compact_model == config::Source::Default);
}

TEST_CASE("MergeConfig: compact_model 配置文件压过默认值") {
    config::FileConfig file;
    file.compact_model = "MiniMax-M3-mini";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.compact_model == "MiniMax-M3-mini");
    CHECK(result->sources.compact_model == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: compact_model 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.compact_model = "env-compact-model";

    config::FileConfig file;
    file.compact_model = "file-compact-model";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.compact_model == "env-compact-model");
    CHECK(result->sources.compact_model == config::Source::LubancodeEnv);
}

// ---------------------------------------------------------------------------
// think:1 级 > 2 级 > 4 级默认值(空串 = 不发这个参数),没有通用 env 这
// 一级。M10 放开成任意字符串——档位是不是认得,留给发请求那一刻(responses
// 原样递、anthropic 查映射表)去判断,这里不拦、不改大小写、原样存。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: think 什么都没设置时留空") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.think.empty());
    CHECK(result->sources.think == config::Source::Default);
}

TEST_CASE("MergeConfig: think 配置文件压过默认值") {
    config::FileConfig file;
    file.think = "medium";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.think == "medium");
    CHECK(result->sources.think == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: think 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.think = "high";

    config::FileConfig file;
    file.think = "medium";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.think == "high");
    CHECK(result->sources.think == config::Source::LubancodeEnv);
}

TEST_CASE("MergeConfig: think 原样存,不改大小写(M10 放开档位后,大小写归一化交给各自 wire 的映射层)") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.think = "HIGH";

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.think == "HIGH");
}

TEST_CASE("MergeConfig: think 是任意字符串都不报错(M10 放开档位,认不认得留给发请求那一刻判断)") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.think = "extreme";

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.think == "extreme");
}

TEST_CASE("MergeConfig: 配置文件里的 think 也是任意字符串都放行") {
    config::FileConfig file;
    file.think = "ultra";
    file.source_path = "/home/user/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.think == "ultra");
}

// ---------------------------------------------------------------------------
// ParseFileConfigJson:context_window/compact_model/think 三个新字段。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: context_window 写成字符串,原样存进 FileConfig") {
    const std::string json = R"({"context_window": "512k"})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    REQUIRE(result->context_window.has_value());
    CHECK(*result->context_window == "512k");
}

TEST_CASE("ParseFileConfigJson: context_window 写成数字,转成字符串存进 FileConfig") {
    const std::string json = R"({"context_window": 128000})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    REQUIRE(result->context_window.has_value());
    CHECK(*result->context_window == "128000");
}

TEST_CASE("ParseFileConfigJson: context_window 类型不对(比如是数组)报错") {
    const std::string json = R"({"context_window": [1, 2, 3]})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("context_window") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: 能解出 compact_model 和 think 字段") {
    const std::string json = R"({"compact_model": "MiniMax-M3-mini", "think": "low"})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    REQUIRE(result->compact_model.has_value());
    CHECK(*result->compact_model == "MiniMax-M3-mini");
    REQUIRE(result->think.has_value());
    CHECK(*result->think == "low");
}

TEST_CASE("ParseFileConfigJson: compact_model 类型不对报错") {
    const std::string json = R"({"compact_model": 123})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("compact_model") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: think 类型不对报错") {
    const std::string json = R"({"think": 123})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("think") != std::string::npos);
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

// ---------------------------------------------------------------------------
// theme:1 级(LUBANCODE_THEME)> 2 级(配置文件)> 4 级默认值(dark),
// 没有通用 env 这一层(第 3 级压根不认 theme 这种 lubancode 专属概念)。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: theme 什么都没设置时,用内置默认值 dark") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.theme == config::kDefaultTheme);
    CHECK(result->sources.theme == config::Source::Default);
}

TEST_CASE("MergeConfig: theme 配置文件压过默认值") {
    config::FileConfig file;
    file.theme = "light";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.theme == "light");
    CHECK(result->sources.theme == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: theme 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.theme = "plain";

    config::FileConfig file;
    file.theme = "light";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.theme == "plain");
    CHECK(result->sources.theme == config::Source::LubancodeEnv);
}

// ---------------------------------------------------------------------------
// system_prompt_file:跟 theme 同一套优先级规则(1 级 > 2 级 > 4 级默认值,
// 默认值是空字符串,表示"没指定,用内置人格")。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: system_prompt_file 什么都没设置时留空") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.system_prompt_file.empty());
    CHECK(result->sources.system_prompt_file == config::Source::Default);
}

TEST_CASE("MergeConfig: system_prompt_file 配置文件压过默认值") {
    config::FileConfig file;
    file.system_prompt_file = "./persona.md";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.system_prompt_file == "./persona.md");
    CHECK(result->sources.system_prompt_file == config::Source::ConfigFile);
}

TEST_CASE("MergeConfig: system_prompt_file 专属 env 压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.system_prompt_file = "./env-persona.md";

    config::FileConfig file;
    file.system_prompt_file = "./file-persona.md";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.system_prompt_file == "./env-persona.md");
    CHECK(result->sources.system_prompt_file == config::Source::LubancodeEnv);
}

TEST_CASE("ParseFileConfigJson: 能解出 theme 和 system_prompt_file 字段") {
    const std::string json = R"({"theme": "light", "system_prompt_file": "./persona.md"})";
    const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
    REQUIRE(result.has_value());
    REQUIRE(result->theme.has_value());
    CHECK(*result->theme == "light");
    REQUIRE(result->system_prompt_file.has_value());
    CHECK(*result->system_prompt_file == "./persona.md");
}

// ---------------------------------------------------------------------------
// ReadSystemPromptFile:真读磁盘文件(--system-prompt 用),UTF-8 文本原样
// 读出来;文件打不开、内容是空的,都要给可读的错误信息。
// ---------------------------------------------------------------------------

namespace {

// 用完即删的临时文件,内容按 UTF-8 原样写入。
class TempPromptFile {
public:
    explicit TempPromptFile(const std::string& content) {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_prompt_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".md");
        std::ofstream file(path_, std::ios::binary);
        file << content;
    }
    ~TempPromptFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::string Utf8Path() const {
        const std::u8string u8 = path_.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("ReadSystemPromptFile: 正常 .md 文件,内容原样读出来") {
    TempPromptFile file("你只用文言文回答问题,言简意赅。");
    const auto result = config::ReadSystemPromptFile(file.Utf8Path());
    REQUIRE(result.has_value());
    CHECK(*result == "你只用文言文回答问题,言简意赅。");
}

TEST_CASE("ReadSystemPromptFile: 文件不存在,报可读的错误") {
    const auto result = config::ReadSystemPromptFile("D:/lubancode/这个人格文件肯定不存在_xyz_123.md");
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("ReadSystemPromptFile: 空文件报错") {
    TempPromptFile file("");
    const auto result = config::ReadSystemPromptFile(file.Utf8Path());
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

// ---------------------------------------------------------------------------
// 配置迁移:旧位置 <dir>/.lubancode.json -> 新位置 <dir>/.lubancode/config.json。
// MigrateConfigFileIfNeeded 吃的是两个具体路径字符串,真在临时目录里读写
// 文件(不是纯函数,但落在一个每个用例独立的临时子目录里,互不干扰,用完
// 就整个删掉)。
// ---------------------------------------------------------------------------

namespace {

// 每个用例一个独立的临时目录,用完整个删掉,几个用例之间不会互相踩脚。
class TempConfigDir {
public:
    TempConfigDir() {
        dir_ = std::filesystem::temp_directory_path() /
               ("lubancode_config_migrate_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }
    ~TempConfigDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string OldPath() const { return (dir_ / ".lubancode.json").string(); }
    std::string NewPath() const { return (dir_ / ".lubancode" / "config.json").string(); }

    void WriteOldFile(const std::string& content) const {
        std::ofstream file(OldPath(), std::ios::binary);
        file << content;
    }

private:
    std::filesystem::path dir_;
};

}  // namespace

TEST_CASE("MigrateConfigFileIfNeeded: 只有旧文件,搬到新位置,内容不变,旧文件消失") {
    TempConfigDir dir;
    const std::string content = R"({"api_key": "sk-old-key-123"})";
    dir.WriteOldFile(content);

    const auto outcome = config::MigrateConfigFileIfNeeded(dir.OldPath(), dir.NewPath());

    CHECK(outcome.effective_path == dir.NewPath());
    REQUIRE(outcome.notice.has_value());
    CHECK(outcome.notice->find(dir.NewPath()) != std::string::npos);

    CHECK(std::filesystem::exists(dir.NewPath()));
    CHECK_FALSE(std::filesystem::exists(dir.OldPath()));

    std::ifstream moved(dir.NewPath(), std::ios::binary);
    const std::string moved_content((std::istreambuf_iterator<char>(moved)), std::istreambuf_iterator<char>());
    CHECK(moved_content == content);
}

TEST_CASE("MigrateConfigFileIfNeeded: 新旧都存在时,用新的,旧的原样留着不动") {
    TempConfigDir dir;
    dir.WriteOldFile(R"({"api_key": "sk-old"})");
    std::filesystem::create_directories(std::filesystem::path(dir.NewPath()).parent_path());
    {
        std::ofstream new_file(dir.NewPath(), std::ios::binary);
        new_file << R"({"api_key": "sk-new"})";
    }

    const auto outcome = config::MigrateConfigFileIfNeeded(dir.OldPath(), dir.NewPath());

    CHECK(outcome.effective_path == dir.NewPath());
    CHECK_FALSE(outcome.notice.has_value());  // 新的已经在了,不算"迁移",不该有通知

    // 旧文件原样留着,没被碰过。
    REQUIRE(std::filesystem::exists(dir.OldPath()));
    std::ifstream old_file(dir.OldPath(), std::ios::binary);
    const std::string old_content((std::istreambuf_iterator<char>(old_file)), std::istreambuf_iterator<char>());
    CHECK(old_content == R"({"api_key": "sk-old"})");

    std::ifstream new_file(dir.NewPath(), std::ios::binary);
    const std::string new_content((std::istreambuf_iterator<char>(new_file)), std::istreambuf_iterator<char>());
    CHECK(new_content == R"({"api_key": "sk-new"})");
}

TEST_CASE("MigrateConfigFileIfNeeded: 新旧都没有,什么都不做,也不报通知") {
    TempConfigDir dir;
    const auto outcome = config::MigrateConfigFileIfNeeded(dir.OldPath(), dir.NewPath());
    CHECK(outcome.effective_path.empty());
    CHECK_FALSE(outcome.notice.has_value());
    CHECK_FALSE(std::filesystem::exists(dir.NewPath()));
}

TEST_CASE("MigrateConfigFileIfNeeded: 建目录、搬文件之后,内容能正常喂给 ParseFileConfigJson") {
    // LoadFileConfig() 本身不吃路径参数(内部直接用 HomeDir()/cwd,没法在
    // 单测里安全地借真实用户主目录摆弄),迁移这一步的文件系统逻辑已经在
    // 上面几条用例里用 MigrateConfigFileIfNeeded 单独测过了;这里补一条端
    // 到端一点的:搬完之后,新位置的文件内容依然是合法配置,能正常解析
    // 出字段,不是被迁移过程弄坏的半截文件。
    TempConfigDir dir;
    dir.WriteOldFile(R"({"base_url": "https://example.com", "api_key": "sk-migrate-test"})");

    const auto outcome = config::MigrateConfigFileIfNeeded(dir.OldPath(), dir.NewPath());
    REQUIRE(outcome.effective_path == dir.NewPath());

    std::ifstream new_file(dir.NewPath(), std::ios::binary);
    const std::string new_content((std::istreambuf_iterator<char>(new_file)), std::istreambuf_iterator<char>());
    const auto parsed = config::ParseFileConfigJson(new_content, dir.NewPath());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->base_url.has_value());
    CHECK(*parsed->base_url == "https://example.com");
    REQUIRE(parsed->api_key.has_value());
    CHECK(*parsed->api_key == "sk-migrate-test");
}

// ---------------------------------------------------------------------------
// websearch:search 段解析 + 合并。只从配置文件来,没配就是空的
// (Configured()=false,web_search 工具不注册)。
// ---------------------------------------------------------------------------

TEST_CASE("ParseSearchConfig: provider + api_key 齐活,三家都认") {
    for (const std::string provider : {"tavily", "brave", "serper"}) {
        nlohmann::json j = {{"provider", provider}, {"api_key", "sk-search-test"}};
        const auto result = config::ParseSearchConfig(j, "/tmp/config.json");
        REQUIRE(result.has_value());
        CHECK(result->provider == provider);
        CHECK(result->api_key == "sk-search-test");
        CHECK(result->Configured());
    }
}

TEST_CASE("ParseSearchConfig: 不是 object 报错") {
    const auto result = config::ParseSearchConfig(nlohmann::json("tavily"), "/tmp/config.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("search") != std::string::npos);
}

TEST_CASE("ParseSearchConfig: provider 不认识报错,错误信息带文件路径") {
    nlohmann::json j = {{"provider", "bing"}, {"api_key", "k"}};
    const auto result = config::ParseSearchConfig(j, "/home/u/.lubancode/config.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("bing") != std::string::npos);
    CHECK(result.error().find("/home/u/.lubancode/config.json") != std::string::npos);
}

TEST_CASE("ParseSearchConfig: 缺 provider / 缺 api_key / api_key 空串,都报错") {
    CHECK_FALSE(config::ParseSearchConfig(nlohmann::json{{"api_key", "k"}}, "p").has_value());
    CHECK_FALSE(config::ParseSearchConfig(nlohmann::json{{"provider", "tavily"}}, "p").has_value());
    CHECK_FALSE(
        config::ParseSearchConfig(nlohmann::json{{"provider", "tavily"}, {"api_key", ""}}, "p").has_value());
}

TEST_CASE("ParseFileConfigJson: search 段解出来,缺省是 nullopt") {
    const auto with_search = config::ParseFileConfigJson(
        R"({"search": {"provider": "brave", "api_key": "sk-b"}})", "/tmp/config.json");
    REQUIRE(with_search.has_value());
    REQUIRE(with_search->search.has_value());
    CHECK(with_search->search->provider == "brave");
    CHECK(with_search->search->api_key == "sk-b");

    const auto without = config::ParseFileConfigJson("{}", "/tmp/config.json");
    REQUIRE(without.has_value());
    CHECK_FALSE(without->search.has_value());
}

TEST_CASE("ParseFileConfigJson: search 段坏了,整个文件解析报错") {
    const auto result = config::ParseFileConfigJson(
        R"({"search": {"provider": "nope", "api_key": "k"}})", "/tmp/config.json");
    CHECK_FALSE(result.has_value());
}

TEST_CASE("MergeConfig: 配置文件里的 search 段原样进最终配置;没写就是未配置") {
    config::FileConfig file;
    file.source_path = "/tmp/config.json";
    config::SearchConfig search;
    search.provider = "serper";
    search.api_key = "sk-s";
    file.search = search;

    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    CHECK(merged->config.search.provider == "serper");
    CHECK(merged->config.search.api_key == "sk-s");
    CHECK(merged->config.search.Configured());

    const auto merged_empty = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(merged_empty.has_value());
    CHECK_FALSE(merged_empty->config.search.Configured());
}

// ---------------------------------------------------------------------------
// tool_search_threshold(延迟挂载阈值):只有配置文件和内置默认值两级。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: tool_search_threshold 正常解析,0 也认,负数/非整数报错") {
    const auto ok = config::ParseFileConfigJson(R"({"tool_search_threshold": 5})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->tool_search_threshold.has_value());
    CHECK(*ok->tool_search_threshold == 5);

    const auto zero = config::ParseFileConfigJson(R"({"tool_search_threshold": 0})", "x.json");
    REQUIRE(zero.has_value());
    CHECK(*zero->tool_search_threshold == 0);

    const auto missing = config::ParseFileConfigJson(R"({})", "x.json");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->tool_search_threshold.has_value());

    CHECK_FALSE(config::ParseFileConfigJson(R"({"tool_search_threshold": -1})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"tool_search_threshold": "20"})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"tool_search_threshold": 2.5})", "x.json").has_value());
}

TEST_CASE("MergeConfig: tool_search_threshold 配置文件压过默认值,没写走默认 20") {
    const auto defaulted = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(defaulted.has_value());
    CHECK(defaulted->config.tool_search_threshold == config::kDefaultToolSearchThreshold);
    CHECK(defaulted->config.tool_search_threshold == 20);
    CHECK(defaulted->sources.tool_search_threshold == config::Source::Default);

    config::FileConfig file;
    file.tool_search_threshold = 5;
    const auto from_file = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(from_file.has_value());
    CHECK(from_file->config.tool_search_threshold == 5);
    CHECK(from_file->sources.tool_search_threshold == config::Source::ConfigFile);
}
