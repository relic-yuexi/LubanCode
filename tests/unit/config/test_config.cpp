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

    CHECK(result->sources.base_url == config::Source::ProjectConfigFile);
    CHECK(result->sources.auth_token == config::Source::ProjectConfigFile);
    CHECK(result->sources.model == config::Source::ProjectConfigFile);
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
    CHECK(result->sources.base_url == config::Source::ProjectConfigFile);

    CHECK(result->config.auth_token == "file-key");
    CHECK(result->sources.auth_token == config::Source::ProjectConfigFile);

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
    CHECK(result->sources.model == config::Source::ProjectConfigFile);

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
    CHECK(file_only->sources.wire == config::Source::ProjectConfigFile);

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
    CHECK(result->sources.max_context_chars == config::Source::ProjectConfigFile);
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
// max_steps_per_turn(旧名 max_turns):待遇跟 max_context_chars 一样(专属
// env / 配置文件 / 默认值三级,没有通用 env)。语义:不配、或者显式配 0,
// 都是无上限;配正整数才是硬上限。负数/非法值不报错,静默忽略落到下一级。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: 什么都没设置时,max_turns 走内置默认值 0(无上限)") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == config::kDefaultMaxStepsPerTurn);
    CHECK(result->config.max_steps_per_turn == 0);
    CHECK(result->sources.max_steps_per_turn == config::Source::Default);
}

TEST_CASE("MergeConfig: max_turns 配置文件压过默认值") {
    config::FileConfig file;
    file.max_turns = 50;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 50);
    CHECK(result->sources.max_steps_per_turn == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: max_turns 配置文件显式写 0,合并结果就是 0(无上限),不当没配") {
    config::FileConfig file;
    file.max_turns = 0;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 0);
    // 来源仍然记成配置文件那一级(不是 Default)——0 是显式配的值。
    CHECK(result->sources.max_steps_per_turn == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: max_turns 专属 env(LUBANCODE_MAX_TURNS)压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.max_turns = 30;

    config::FileConfig file;
    file.max_turns = 50;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 30);
    CHECK(result->sources.max_steps_per_turn == config::Source::LubancodeEnv);
}

TEST_CASE("ParseFileConfigJson: max_turns 正整数正常解析") {
    const auto ok = config::ParseFileConfigJson(R"({"max_turns": 50})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->max_turns.has_value());
    CHECK(*ok->max_turns == 50);
}

// ---------------------------------------------------------------------------
// max_steps_per_turn <-> max_turns 兼容期双读(命名规范第二批):新名优先;
// 同现同值按新名收账;同现异值明报冲突(取新名,不暗取);旧名单独出现
// 映射到同一字段并打一次性弃用提示;0 = 不限步,两代同义。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: max_steps_per_turn 新键正常解析,与旧键各自独立读入") {
    const auto ok = config::ParseFileConfigJson(R"({"max_steps_per_turn": 40})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->max_steps_per_turn.has_value());
    CHECK(*ok->max_steps_per_turn == 40);
    CHECK_FALSE(ok->max_turns.has_value());
}

TEST_CASE("MergeConfig: 新名单独出现,生效且不打弃用提示") {
    config::FileConfig file;
    file.max_steps_per_turn = 40;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 40);
    CHECK(result->sources.max_steps_per_turn == config::Source::ProjectConfigFile);
    CHECK(result->deprecation_notices.empty());
}

TEST_CASE("MergeConfig: 旧名单独出现,映射生效并打一条弃用提示") {
    config::FileConfig file;
    file.max_turns = 25;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 25);  // 旧名映射到同一字段
    CHECK(result->sources.max_steps_per_turn == config::Source::ProjectConfigFile);
    REQUIRE(result->deprecation_notices.size() == 1);
    CHECK(result->deprecation_notices[0].find("max_turns=25") != std::string::npos);
    CHECK(result->deprecation_notices[0].find("已弃用") != std::string::npos);
}

TEST_CASE("MergeConfig: 新旧同现同值,按新名收账并提示弃用") {
    config::FileConfig file;
    file.max_steps_per_turn = 40;
    file.max_turns = 40;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 40);
    REQUIRE(result->deprecation_notices.size() == 1);
    CHECK(result->deprecation_notices[0].find("同值") != std::string::npos);
}

TEST_CASE("MergeConfig: 新旧同现异值,明报冲突,取新名不暗取") {
    config::FileConfig file;
    file.max_steps_per_turn = 40;
    file.max_turns = 15;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 40);  // 新名优先
    REQUIRE(result->deprecation_notices.size() == 1);
    // 冲突信息把两个值都摆出来,不暗取一边。
    CHECK(result->deprecation_notices[0].find("冲突") != std::string::npos);
    CHECK(result->deprecation_notices[0].find("max_steps_per_turn=40") != std::string::npos);
    CHECK(result->deprecation_notices[0].find("max_turns=15") != std::string::npos);
}

TEST_CASE("MergeConfig: 旧名 0 = 不限步,两代同义(显式无上限不当没配)") {
    config::FileConfig file;
    file.max_turns = 0;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 0);
    CHECK(result->sources.max_steps_per_turn == config::Source::ProjectConfigFile);
    REQUIRE(result->deprecation_notices.size() == 1);
}

TEST_CASE("MergeConfig: env 新名压过配置文件旧名(层级照旧,env > 文件)") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.max_steps_per_turn = 30;

    config::FileConfig file;
    file.max_turns = 50;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 30);
    CHECK(result->sources.max_steps_per_turn == config::Source::LubancodeEnv);
    // 文件里旧名的弃用提示照打(读了旧名这件事要记清)。
    REQUIRE(result->deprecation_notices.size() == 1);
}

TEST_CASE("MergeConfig: env 新旧两个变量同现异值,明报冲突取新名") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.max_steps_per_turn = 30;
    lubancode_env.max_turns = 20;

    const auto result = config::MergeConfig(lubancode_env, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.max_steps_per_turn == 30);
    CHECK(result->sources.max_steps_per_turn == config::Source::LubancodeEnv);
    REQUIRE(result->deprecation_notices.size() == 1);
    CHECK(result->deprecation_notices[0].find("冲突") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: subagent 段新键 max_steps_per_turn 正常解析") {
    const auto ok = config::ParseFileConfigJson(R"({"subagent": {"max_steps_per_turn": 25}})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->subagent_max_steps_per_turn.has_value());
    CHECK(*ok->subagent_max_steps_per_turn == 25);
    CHECK_FALSE(ok->subagent_max_turns.has_value());
}

TEST_CASE("MergeConfig: subagent 段新旧同现异值,明报冲突取新名") {
    config::FileConfig file;
    file.subagent_max_steps_per_turn = 20;
    file.subagent_max_turns = 8;
    file.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    REQUIRE(result->config.subagent.max_steps_per_turn.has_value());
    CHECK(*result->config.subagent.max_steps_per_turn == 20);
    CHECK(result->sources.subagent == config::Source::ProjectConfigFile);
    REQUIRE(result->deprecation_notices.size() == 1);
    CHECK(result->deprecation_notices[0].find("subagent") != std::string::npos);
    CHECK(result->deprecation_notices[0].find("冲突") != std::string::npos);
}

// ---------------------------------------------------------------------------
// agent 段与 subagent.max_output_tokens(规格"子代理与 MainAgent 同级"
// 根因一):输出上限从配置三级来,null = unset(交服务端默认/provider/
// 目录声明),subagent 段未写就继承 agent 段,不落回编译期魔数。
// length_continuations 默认 1(实测),坏值静默跳过。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: agent 段 max_output_tokens 与 length_continuations") {
    const auto ok = config::ParseFileConfigJson(
        R"({"agent": {"max_output_tokens": 32768, "length_continuations": 2}})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->agent_max_output_tokens.has_value());
    CHECK(*ok->agent_max_output_tokens == 32768);
    REQUIRE(ok->agent_length_continuations.has_value());
    CHECK(*ok->agent_length_continuations == 2);

    // null 与缺失同义(= unset,不压过任何一级声明);0/负数/坏类型静默跳过。
    const auto null_field = config::ParseFileConfigJson(R"({"agent": {"max_output_tokens": null}})", "x.json");
    REQUIRE(null_field.has_value());
    CHECK_FALSE(null_field->agent_max_output_tokens.has_value());
    const auto bad = config::ParseFileConfigJson(R"({"agent": {"max_output_tokens": 0, "length_continuations": -1}})",
                                                 "x.json");
    REQUIRE(bad.has_value());
    CHECK_FALSE(bad->agent_max_output_tokens.has_value());
    CHECK_FALSE(bad->agent_length_continuations.has_value());
    // 段不是 object:整段跳过,不算错。
    const auto bad_segment = config::ParseFileConfigJson(R"({"agent": 3})", "x.json");
    REQUIRE(bad_segment.has_value());
    CHECK_FALSE(bad_segment->agent_max_output_tokens.has_value());
}

TEST_CASE("ParseFileConfigJson: subagent.max_output_tokens(null = 继承 agent 段)") {
    const auto ok = config::ParseFileConfigJson(R"({"subagent": {"max_output_tokens": 8192}})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->subagent_max_output_tokens.has_value());
    CHECK(*ok->subagent_max_output_tokens == 8192);
    // null 与缺失同义:继承 agent 段(运行时 BuildSubagentRuntimeProfile 算)。
    const auto null_field = config::ParseFileConfigJson(R"({"subagent": {"max_output_tokens": null}})", "x.json");
    REQUIRE(null_field.has_value());
    CHECK_FALSE(null_field->subagent_max_output_tokens.has_value());
}

TEST_CASE("MergeConfig: agent 段项目级压全局,都没写 = unset + 默认续跑 1 次") {
    config::FileConfig project;
    project.agent_max_output_tokens = 65536;
    project.source_path = "/tmp/project/.lubancode/config.json";
    config::FileConfig global;
    global.agent_max_output_tokens = 16384;
    global.agent_length_continuations = 3;
    global.source_path = "/tmp/home/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(result.has_value());
    REQUIRE(result->config.agent.max_output_tokens.has_value());
    CHECK(*result->config.agent.max_output_tokens == 65536);  // 项目级压全局
    CHECK(result->sources.agent == config::Source::ProjectConfigFile);
    CHECK(result->config.agent.length_continuations == 3);  // 全局的字段各回各级

    const auto none = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global, EmptyGenericEnv());
    REQUIRE(none.has_value());
    REQUIRE(none->config.agent.max_output_tokens.has_value());
    CHECK(*none->config.agent.max_output_tokens == 16384);
    CHECK(none->sources.agent == config::Source::GlobalConfigFile);

    // 全都没有:unset + 默认续跑次数,绝无 4096。
    config::FileConfig empty;
    empty.source_path = "/tmp/.lubancode/config.json";
    const auto unset = config::MergeConfig(EmptyLubancodeEnv(), empty, EmptyGenericEnv());
    REQUIRE(unset.has_value());
    CHECK(unset->config.agent.max_output_tokens == std::nullopt);
    CHECK(unset->config.agent.length_continuations == config::kDefaultLengthContinuations);
    CHECK(unset->config.subagent.max_output_tokens == std::nullopt);
    CHECK(unset->sources.agent == config::Source::Default);
}

TEST_CASE("ParseFileConfigJson/MergeConfig: subagent.max_depth 与 max_active(派工治理)") {
    const auto ok = config::ParseFileConfigJson(R"({"subagent": {"max_depth": 2, "max_active": 4}})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->subagent_max_depth.has_value());
    CHECK(*ok->subagent_max_depth == 2);
    REQUIRE(ok->subagent_max_active.has_value());
    CHECK(*ok->subagent_max_active == 4);
    // 坏值(0/负数/超界)静默跳过。
    const auto bad = config::ParseFileConfigJson(R"({"subagent": {"max_depth": 0, "max_active": -1}})", "x.json");
    REQUIRE(bad.has_value());
    CHECK_FALSE(bad->subagent_max_depth.has_value());
    CHECK_FALSE(bad->subagent_max_active.has_value());

    // 合并:项目级压全局,没写 = nullopt(运行时用公开默认值)。
    config::FileConfig global;
    global.subagent_max_depth = 5;
    global.source_path = "/tmp/home/.lubancode/config.json";
    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    REQUIRE(merged->config.subagent.max_depth.has_value());
    CHECK(*merged->config.subagent.max_depth == 5);
    CHECK(merged->config.subagent.max_active == std::nullopt);
}

// ---------------------------------------------------------------------------
// subagent.wall_clock_timeout_secs(规格《子代理活跃度不可见与疑似挂死》):
// 整轮墙钟兜底,0 = 不限;坏值静默跳过,合并项目级压全局,没写 = nullopt
// (运行时用 kDefaultSubagentWallClockTimeoutSecs = 1800)。
// ---------------------------------------------------------------------------
TEST_CASE("ParseFileConfigJson/MergeConfig: subagent.wall_clock_timeout_secs(墙钟兜底)") {
    const auto ok = config::ParseFileConfigJson(R"({"subagent": {"wall_clock_timeout_secs": 600}})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->subagent_wall_clock_timeout_secs.has_value());
    CHECK(*ok->subagent_wall_clock_timeout_secs == 600);
    // 0 = 不限,合法显式值。
    const auto zero = config::ParseFileConfigJson(R"({"subagent": {"wall_clock_timeout_secs": 0}})", "x.json");
    REQUIRE(zero.has_value());
    REQUIRE(zero->subagent_wall_clock_timeout_secs.has_value());
    CHECK(*zero->subagent_wall_clock_timeout_secs == 0);
    // 坏值(负数/非整数/超界)静默跳过。
    const auto bad =
        config::ParseFileConfigJson(R"({"subagent": {"wall_clock_timeout_secs": -5}})", "x.json");
    REQUIRE(bad.has_value());
    CHECK_FALSE(bad->subagent_wall_clock_timeout_secs.has_value());
    const auto bad_type =
        config::ParseFileConfigJson(R"({"subagent": {"wall_clock_timeout_secs": "30min"}})", "x.json");
    REQUIRE(bad_type.has_value());
    CHECK_FALSE(bad_type->subagent_wall_clock_timeout_secs.has_value());

    // 合并:项目级压全局,都没写 = nullopt。
    config::FileConfig global;
    global.subagent_wall_clock_timeout_secs = 900;
    global.source_path = "/tmp/home/.lubancode/config.json";
    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    REQUIRE(merged->config.subagent.wall_clock_timeout_secs.has_value());
    CHECK(*merged->config.subagent.wall_clock_timeout_secs == 900);
    const auto merged_empty = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, std::nullopt,
                                                  EmptyGenericEnv());
    REQUIRE(merged_empty.has_value());
    CHECK(merged_empty->config.subagent.wall_clock_timeout_secs == std::nullopt);
}

// ---------------------------------------------------------------------------
// subagent.max_steps_per_turn(旧名 subagent.max_turns;0.30.x"失败预算"单):
// 子代理不再暗藏硬闸——预算从配置来,首选 subagent 段,未设继承主代理的
// 预算;0 全路一致不限步。待遇同 hooks:只从配置文件来(项目级压全局),
// 没有 env/默认两级。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: subagent.max_turns 正常解析;坏段/坏值/负数静默跳过") {
    const auto ok = config::ParseFileConfigJson(R"({"subagent": {"max_turns": 25}})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->subagent_max_turns.has_value());
    CHECK(*ok->subagent_max_turns == 25);
    // 显式 0 = 子代理不限轮,是合法值,不当没配。
    const auto zero = config::ParseFileConfigJson(R"({"subagent": {"max_turns": 0}})", "x.json");
    REQUIRE(zero.has_value());
    REQUIRE(zero->subagent_max_turns.has_value());
    CHECK(*zero->subagent_max_turns == 0);
    // 段不是 object / 字段不是整数 / 负数:静默跳过(救命阀字段,写错不拦人)。
    const auto bad_segment = config::ParseFileConfigJson(R"({"subagent": 3})", "x.json");
    REQUIRE(bad_segment.has_value());
    CHECK_FALSE(bad_segment->subagent_max_turns.has_value());
    const auto bad_type = config::ParseFileConfigJson(R"({"subagent": {"max_turns": "40"}})", "x.json");
    REQUIRE(bad_type.has_value());
    CHECK_FALSE(bad_type->subagent_max_turns.has_value());
    const auto negative = config::ParseFileConfigJson(R"({"subagent": {"max_turns": -4}})", "x.json");
    REQUIRE(negative.has_value());
    CHECK_FALSE(negative->subagent_max_turns.has_value());
}

TEST_CASE("MergeConfig: subagent.max_turns 未设时留 nullopt(运行时继承 max_turns)") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK_FALSE(result->config.subagent.max_steps_per_turn.has_value());
    CHECK(result->sources.subagent == config::Source::Default);
}

TEST_CASE("MergeConfig: subagent.max_turns 项目级压全局,与 max_turns 互不干扰") {
    config::FileConfig global;
    global.max_turns = 60;
    global.subagent_max_turns = 12;
    global.source_path = "/home/.lubancode/config.json";
    config::FileConfig project;
    project.max_turns = 50;
    project.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(result.has_value());
    // 项目级只写了 max_turns:subagent 段回退全局的 12;max_turns 用项目级 50。
    REQUIRE(result->config.subagent.max_steps_per_turn.has_value());
    CHECK(*result->config.subagent.max_steps_per_turn == 12);
    CHECK(result->sources.subagent == config::Source::GlobalConfigFile);
    CHECK(result->config.max_steps_per_turn == 50);
    CHECK(result->sources.max_steps_per_turn == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: subagent.max_turns 显式 0 是显式不限轮,不当没配") {
    config::FileConfig project;
    project.max_turns = 40;
    project.subagent_max_turns = 0;
    project.source_path = "/tmp/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), project, std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    REQUIRE(result->config.subagent.max_steps_per_turn.has_value());
    CHECK(*result->config.subagent.max_steps_per_turn == 0);  // 子代理不限轮,主代理仍 40
    CHECK(result->config.max_steps_per_turn == 40);
    CHECK(result->sources.subagent == config::Source::ProjectConfigFile);
}

TEST_CASE("ParseFileConfigJson: memory 对象能解析，坏类型会报错") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"memory":{"enabled":true,"use":false,"generate":true,"max_index_bytes":1000,"max_retrieval_bytes":2000,"max_results":3}})",
        "x.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->memory.has_value());
    CHECK(parsed->memory->enabled == true);
    CHECK(parsed->memory->use == false);
    CHECK(parsed->memory->max_results == 3);

    CHECK_FALSE(config::ParseFileConfigJson(R"({"memory":true})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"memory":{"max_results":0}})", "x.json").has_value());
}

TEST_CASE("MergeConfig: memory 默认关闭，项目配置不能自行开启") {
    const auto defaults = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, std::nullopt,
                                               EmptyGenericEnv());
    REQUIRE(defaults.has_value());
    CHECK_FALSE(defaults->config.memory.enabled);

    config::FileConfig project;
    project.source_path = "project.json";
    config::MemoryFileConfig project_memory;
    project_memory.enabled = true;
    project.memory = project_memory;
    const auto project_only = config::MergeConfig(EmptyLubancodeEnv(), project, std::nullopt,
                                                   EmptyGenericEnv());
    REQUIRE(project_only.has_value());
    CHECK_FALSE(project_only->config.memory.enabled);
}

TEST_CASE("MergeConfig: 全局打开 memory，项目可以收窄并关闭") {
    config::FileConfig global;
    global.source_path = "global.json";
    config::MemoryFileConfig global_memory;
    global_memory.enabled = true;
    global_memory.max_results = 6;
    global.memory = global_memory;

    config::FileConfig project;
    project.source_path = "project.json";
    config::MemoryFileConfig project_memory;
    project_memory.use = false;
    project_memory.max_results = 2;
    project.memory = project_memory;
    const auto narrowed = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(narrowed.has_value());
    CHECK(narrowed->config.memory.enabled);
    CHECK_FALSE(narrowed->config.memory.use);
    CHECK(narrowed->config.memory.max_results == 2);

    project.memory->enabled = false;
    const auto disabled = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(disabled.has_value());
    CHECK_FALSE(disabled->config.memory.enabled);
}

TEST_CASE("MergeConfig: memory.user_enabled 只认全局授权,项目只能收窄成关") {
    // 默认关。
    const auto defaults = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, std::nullopt,
                                               EmptyGenericEnv());
    REQUIRE(defaults.has_value());
    CHECK_FALSE(defaults->config.memory.user_enabled);

    // 项目配置写 true 不生效:用户级记忆的授权只能在全局配置给。
    config::FileConfig project;
    project.source_path = "project.json";
    config::MemoryFileConfig project_memory;
    project_memory.enabled = true;
    project_memory.user_enabled = true;
    project.memory = project_memory;
    const auto project_only = config::MergeConfig(EmptyLubancodeEnv(), project, std::nullopt,
                                                   EmptyGenericEnv());
    REQUIRE(project_only.has_value());
    CHECK_FALSE(project_only->config.memory.user_enabled);

    // 全局授权后,项目可以收窄成关。
    config::FileConfig global;
    global.source_path = "global.json";
    config::MemoryFileConfig global_memory;
    global_memory.enabled = true;
    global_memory.user_enabled = true;
    global.memory = global_memory;
    const auto granted = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global,
                                              EmptyGenericEnv());
    REQUIRE(granted.has_value());
    CHECK(granted->config.memory.user_enabled);

    config::MemoryFileConfig narrow;
    narrow.user_enabled = false;
    project.memory = narrow;
    const auto narrowed = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(narrowed.has_value());
    CHECK_FALSE(narrowed->config.memory.user_enabled);

    // 坏值拒绝。
    CHECK_FALSE(config::ParseFileConfigJson(R"({"memory":{"user_enabled":"yes"}})", "x.json").has_value());
}

TEST_CASE("MergeConfig: memory.learn 只认三档,项目配置只能收窄") {
    // 坏值解析直接报错。
    CHECK_FALSE(config::ParseFileConfigJson(R"({"memory":{"learn":"always"}})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"memory":{"learn":1}})", "x.json").has_value());
    const auto parsed = config::ParseFileConfigJson(R"({"memory":{"learn":"auto"}})", "x.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->memory.has_value());
    CHECK(*parsed->memory->learn == "auto");

    // 全局显式授权 auto,项目收窄回 review。
    config::FileConfig global;
    global.source_path = "global.json";
    config::MemoryFileConfig global_memory;
    global_memory.enabled = true;
    global_memory.learn = "auto";
    global.memory = global_memory;
    config::FileConfig project;
    project.source_path = "project.json";
    config::MemoryFileConfig project_memory;
    project_memory.learn = "review";
    project.memory = project_memory;
    const auto narrowed = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(narrowed.has_value());
    CHECK(narrowed->config.memory.learn == "review");

    // 全局只给 review,项目想升 auto:合并后仍是 review。
    global.memory->learn = "review";
    project.memory->learn = "auto";
    const auto capped = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(capped.has_value());
    CHECK(capped->config.memory.learn == "review");

    // 全局 auto、项目不写 learn:auto 保持。
    global.memory->learn = "auto";
    project.memory.reset();
    const auto kept = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(kept.has_value());
    CHECK(kept->config.memory.learn == "auto");

    // 老写法 generate=false 等价 learn=off。
    global.memory->learn = "auto";
    config::MemoryFileConfig legacy;
    legacy.generate = false;
    project.memory = legacy;
    const auto legacy_off = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(legacy_off.has_value());
    CHECK(legacy_off->config.memory.learn == "off");
}

TEST_CASE("ParseFileConfigJson: max_turns 缺省时是 nullopt") {
    const auto missing = config::ParseFileConfigJson("{}", "x.json");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->max_turns.has_value());
}

TEST_CASE("ParseFileConfigJson: max_turns 显式写 0 是合法值(显式无上限),不是 nullopt") {
    const auto zero = config::ParseFileConfigJson(R"({"max_turns": 0})", "x.json");
    REQUIRE(zero.has_value());  // 不报错
    REQUIRE(zero->max_turns.has_value());  // 落进去了,不是"当没设"
    CHECK(*zero->max_turns == 0);
}

TEST_CASE("ParseFileConfigJson: max_turns 负数或类型不对时静默忽略(留 nullopt),不报错") {
    const auto negative = config::ParseFileConfigJson(R"({"max_turns": -5})", "x.json");
    REQUIRE(negative.has_value());
    CHECK_FALSE(negative->max_turns.has_value());

    const auto wrong_type = config::ParseFileConfigJson(R"({"max_turns": "50"})", "x.json");
    REQUIRE(wrong_type.has_value());
    CHECK_FALSE(wrong_type->max_turns.has_value());

    const auto float_value = config::ParseFileConfigJson(R"({"max_turns": 2.5})", "x.json");
    REQUIRE(float_value.has_value());
    CHECK_FALSE(float_value->max_turns.has_value());
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

TEST_CASE("status_panel: 解析字段顺序与自定义分隔符") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"status_panel":{"items":["model","cwd","git_branch","provider","effort"],"separator":" | "}})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->status_panel.has_value());
    CHECK(parsed->status_panel->items ==
          std::vector<std::string>{"model", "cwd", "git_branch", "provider", "effort"});
    CHECK(parsed->status_panel->separator == " | ");
}

TEST_CASE("status_panel: 坏字段、重复字段与换行分隔符都拦下") {
    CHECK_FALSE(config::ParseFileConfigJson(
                    R"({"status_panel":{"items":["model","no_such_field"]}})", "bad.json")
                    .has_value());
    CHECK_FALSE(config::ParseFileConfigJson(
                    R"({"status_panel":{"items":["cwd","cwd"]}})", "bad.json")
                    .has_value());
    CHECK_FALSE(config::ParseFileConfigJson(
                    "{\"status_panel\":{\"separator\":\"\\n\"}}", "bad.json")
                    .has_value());
}

TEST_CASE("status_panel: 项目级整段压过全局，没配置走内置字段") {
    config::FileConfig project;
    project.source_path = "project.json";
    project.status_panel = config::StatusPanelConfig{{"cwd", "git_branch"}, " / "};
    config::FileConfig global;
    global.source_path = "global.json";
    global.status_panel = config::StatusPanelConfig{{"model"}, " | "};

    const auto merged = config::MergeConfig({}, project, global, {});
    REQUIRE(merged.has_value());
    CHECK(merged->config.status_panel.items == std::vector<std::string>{"cwd", "git_branch"});
    CHECK(merged->config.status_panel.separator == " / ");
    CHECK(merged->sources.status_panel == config::Source::ProjectConfigFile);

    const auto defaults = config::MergeConfig({}, std::nullopt, std::nullopt, {});
    REQUIRE(defaults.has_value());
    CHECK(defaults->config.status_panel.items ==
          std::vector<std::string>{"permission_mode", "model", "effort", "cwd", "git_branch", "context", "tokens"});
    CHECK(defaults->sources.status_panel == config::Source::Default);
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
    CHECK(result->sources.context_window_tokens == config::Source::ProjectConfigFile);
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
    CHECK(result->sources.compact_model == config::Source::ProjectConfigFile);
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
    CHECK(result->sources.think == config::Source::ProjectConfigFile);
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
// soul(0.16.x 魂法分家):1 级(LUBANCODE_SOUL)> 2 级 > 4 级默认值
// (空串 = 用主目录 SOUL.md),没有通用 env 这一级。名字对不对得上文件,
// 这里不校验(启动时按名字找,找不到打警告、魂不生效)。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: soul 什么都没设置时留空(= 用 SOUL.md)") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.soul.empty());
    CHECK(result->sources.soul == config::Source::Default);
}

TEST_CASE("MergeConfig: soul 配置文件压过默认值") {
    config::FileConfig file;
    file.soul = "wenyan";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.soul == "wenyan");
    CHECK(result->sources.soul == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: soul 专属 env(LUBANCODE_SOUL)压过配置文件") {
    config::LubancodeEnvValues lubancode_env;
    lubancode_env.soul = "pirate";

    config::FileConfig file;
    file.soul = "wenyan";
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(lubancode_env, file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.soul == "pirate");
    CHECK(result->sources.soul == config::Source::LubancodeEnv);
}

TEST_CASE("ParseFileConfigJson: 能解出 soul 字段;类型不对报错") {
    const auto ok = config::ParseFileConfigJson(R"({"soul": "wenyan"})", "/tmp/.lubancode.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->soul.has_value());
    CHECK(*ok->soul == "wenyan");

    const auto bad = config::ParseFileConfigJson(R"({"soul": 42})", "/tmp/.lubancode.json");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("soul") != std::string::npos);
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
// compact_partition_count(Compact 四分区单·阶段 1):只从配置文件来(项目级
// 压全局),默认 4,取值域 2..8,越界报错不夹值(§八)。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig: compact_partition_count 什么都没设置时用内置默认值 4") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.compact_partition_count == config::kDefaultCompactPartitionCount);
    CHECK(result->config.compact_partition_count == 4);
    CHECK(result->sources.compact_partition_count == config::Source::Default);
}

TEST_CASE("MergeConfig: compact_partition_count 配置文件压过默认值,来源记配置文件") {
    config::FileConfig file;
    file.compact_partition_count = 6;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.compact_partition_count == 6);
    CHECK(result->sources.compact_partition_count == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: compact_partition_count 边界值 2 与 8 都合法") {
    {
        config::FileConfig file;
        file.compact_partition_count = config::kMinCompactPartitionCount;
        file.source_path = "/tmp/.lubancode.json";
        const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
        REQUIRE(result.has_value());
        CHECK(result->config.compact_partition_count == 2);
    }
    {
        config::FileConfig file;
        file.compact_partition_count = config::kMaxCompactPartitionCount;
        file.source_path = "/tmp/.lubancode.json";
        const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
        REQUIRE(result.has_value());
        CHECK(result->config.compact_partition_count == 8);
    }
}

TEST_CASE("MergeConfig: compact_partition_count 越界报错,不静默夹值,错误信息带文件路径") {
    config::FileConfig file;
    file.compact_partition_count = 9;
    file.source_path = "/home/user/.lubancode.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("compact_partition_count") != std::string::npos);
    CHECK(result.error().find("/home/user/.lubancode.json") != std::string::npos);

    config::FileConfig zero;
    zero.compact_partition_count = 1;
    zero.source_path = "/home/user/.lubancode.json";
    const auto too_small = config::MergeConfig(EmptyLubancodeEnv(), zero, EmptyGenericEnv());
    REQUIRE_FALSE(too_small.has_value());
    CHECK(too_small.error().find("2..8") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: compact_partition_count 解出整数;类型不对报错") {
    {
        const std::string json = R"({"compact_partition_count": 4})";
        const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
        REQUIRE(result.has_value());
        REQUIRE(result->compact_partition_count.has_value());
        CHECK(*result->compact_partition_count == 4);
    }
    {
        const std::string json = R"({"compact_partition_count": "four"})";
        const auto result = config::ParseFileConfigJson(json, "/tmp/.lubancode.json");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("compact_partition_count") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// ToString(Source):--config 诊断输出用的中文说法,每种来源都要有说法。
// 配置文件拆成项目级/全局两级,两条都得有独立、非空、彼此不同的说法。
// ---------------------------------------------------------------------------

TEST_CASE("ToString(Source): 每种来源都有非空的中文说法,项目级/全局各自区分") {
    CHECK_FALSE(config::ToString(config::Source::LubancodeEnv).empty());
    CHECK_FALSE(config::ToString(config::Source::ProjectConfigFile).empty());
    CHECK_FALSE(config::ToString(config::Source::GlobalConfigFile).empty());
    CHECK_FALSE(config::ToString(config::Source::GenericEnv).empty());
    CHECK_FALSE(config::ToString(config::Source::Default).empty());
    // 项目级与全局的说法不能一样,不然 /config 看不出这字段到底来自哪一级。
    CHECK(config::ToString(config::Source::ProjectConfigFile) !=
          config::ToString(config::Source::GlobalConfigFile));
}

TEST_CASE("环境变量把端点换离 active provider 时标作 unbound") {
    config::Config cfg;
    config::ProviderConfig provider;
    provider.name = "preset-a";
    provider.wire = config::Wire::Responses;
    provider.base_url = "https://preset-a.example/v1";
    provider.model = "gpt-5.6";
    cfg.providers.push_back(provider);
    cfg.active_provider = "preset-a";
    cfg.wire = config::Wire::Anthropic;
    cfg.base_url = "http://localhost:8001";
    cfg.model = "MiniCPM5-1B";
    config::ConfigSources sources;
    sources.wire = config::Source::LubancodeEnv;
    sources.base_url = config::Source::LubancodeEnv;
    sources.model = config::Source::LubancodeEnv;

    CHECK(config::EnvironmentOverridesActiveProvider(cfg, sources, "preset-a"));
    CHECK(config::BoundProviderName(cfg, "preset-a").empty());
    cfg.wire = provider.wire;
    cfg.base_url = provider.base_url;
    cfg.model = provider.model;
    CHECK_FALSE(config::EnvironmentOverridesActiveProvider(cfg, sources, "preset-a"));
    CHECK(config::BoundProviderName(cfg, "preset-a") == "preset-a");
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
    CHECK(result->sources.theme == config::Source::ProjectConfigFile);
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
    CHECK(result->sources.system_prompt_file == config::Source::ProjectConfigFile);
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

TEST_CASE("UpdateSoulInConfigFile: 只改 soul 字段,别的字段(含不认得的)原样保留") {
    TempPromptFile file(R"({"model": "MiniMax-M3", "自定义字段": 42})");
    const auto updated = config::UpdateSoulInConfigFile(file.Utf8Path(), "wenyan");
    REQUIRE(updated.has_value());

    std::ifstream in(file.Utf8Path(), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"soul\"") != std::string::npos);
    CHECK(content.find("wenyan") != std::string::npos);
    CHECK(content.find("MiniMax-M3") != std::string::npos);
    CHECK(content.find("自定义字段") != std::string::npos);
}

TEST_CASE("UpdateSoulInConfigFile: 能把 soul 字段改成 off,覆盖掉旧的魂名") {
    // 对应 /soul off 持久化那条路:配置里原先存着旧魂名(比如 catgirl),
    // 答 y 之后要能真把它改成 off,而不是原样留着(这正是本单要修的 bug)。
    TempPromptFile file(R"({"soul": "catgirl"})");
    const auto updated = config::UpdateSoulInConfigFile(file.Utf8Path(), "off");
    REQUIRE(updated.has_value());

    std::ifstream in(file.Utf8Path(), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"soul\": \"off\"") != std::string::npos);
    CHECK(content.find("catgirl") == std::string::npos);
}

TEST_CASE("UpdateSoulInConfigFile: 文件不是合法 JSON,报错不写") {
    TempPromptFile file("这不是 JSON");
    const auto updated = config::UpdateSoulInConfigFile(file.Utf8Path(), "wenyan");
    REQUIRE_FALSE(updated.has_value());
    CHECK_FALSE(updated.error().empty());
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
    // LoadFileConfigs() 本身不吃路径参数(内部直接用 HomeDir()/cwd,没法在
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
    CHECK(from_file->sources.tool_search_threshold == config::Source::ProjectConfigFile);
}

// ---------------------------------------------------------------------------
// M11(网络超时):connect_timeout_ms / stream_idle_timeout_secs /
// request_timeout_secs 三个字段,待遇跟 tool_search_threshold 一样——只有
// 配置文件(项目级 > 全局)和内置默认值两级,没有环境变量这一级。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: 三个超时字段正常解析,负数/零/非整数都报错") {
    const auto ok = config::ParseFileConfigJson(
        R"({"connect_timeout_ms": 5000, "stream_idle_timeout_secs": 30, "request_timeout_secs": 10})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->connect_timeout_ms.has_value());
    REQUIRE(ok->stream_idle_timeout_secs.has_value());
    REQUIRE(ok->request_timeout_secs.has_value());
    CHECK(*ok->connect_timeout_ms == 5000);
    CHECK(*ok->stream_idle_timeout_secs == 30);
    CHECK(*ok->request_timeout_secs == 10);

    const auto missing = config::ParseFileConfigJson(R"({})", "x.json");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->connect_timeout_ms.has_value());
    CHECK_FALSE(missing->stream_idle_timeout_secs.has_value());
    CHECK_FALSE(missing->request_timeout_secs.has_value());

    // 三个字段都是"正整数"(跟 tool_search_threshold 不一样,0 不合法——
    // 0 毫秒/0 秒的超时没有意义)。
    CHECK_FALSE(config::ParseFileConfigJson(R"({"connect_timeout_ms": 0})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"connect_timeout_ms": -1})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"connect_timeout_ms": "5000"})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"stream_idle_timeout_secs": 0})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"stream_idle_timeout_secs": 2.5})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"request_timeout_secs": -5})", "x.json").has_value());
}

// ---------------------------------------------------------------------------
// 流式请求硬墙钟(cpr 并发挂死单):request_hard_timeout_secs。与上面三个
// 超时字段同住一块,但 0 是合法值(显式不设墙)——解析收非负整数,合并
// 走同一套"项目级 > 全局 > 默认"三级。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: request_hard_timeout_secs 解析,0 合法(不设墙),负数/非整数报错") {
    const auto ok = config::ParseFileConfigJson(
        R"({"request_hard_timeout_secs": 600})", "x.json");
    REQUIRE(ok.has_value());
    REQUIRE(ok->request_hard_timeout_secs.has_value());
    CHECK(*ok->request_hard_timeout_secs == 600);

    const auto zero = config::ParseFileConfigJson(R"({"request_hard_timeout_secs": 0})", "x.json");
    REQUIRE(zero.has_value());
    REQUIRE(zero->request_hard_timeout_secs.has_value());
    CHECK(*zero->request_hard_timeout_secs == 0);  // 0 = 显式不设墙,不是"没写"

    const auto missing = config::ParseFileConfigJson(R"({})", "x.json");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->request_hard_timeout_secs.has_value());

    CHECK_FALSE(config::ParseFileConfigJson(R"({"request_hard_timeout_secs": -1})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"request_hard_timeout_secs": 2.5})", "x.json").has_value());
    CHECK_FALSE(config::ParseFileConfigJson(R"({"request_hard_timeout_secs": "600"})", "x.json").has_value());
}

TEST_CASE("MergeConfig: request_hard_timeout_secs 项目级压全局压默认,显式 0 也照收") {
    const auto defaulted = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(defaulted.has_value());
    CHECK(defaulted->config.request_hard_timeout_secs == config::kDefaultRequestHardTimeoutSecs);
    CHECK(defaulted->sources.request_hard_timeout_secs == config::Source::Default);

    config::FileConfig file;
    file.request_hard_timeout_secs = 600;
    const auto from_file = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(from_file.has_value());
    CHECK(from_file->config.request_hard_timeout_secs == 600);
    CHECK(from_file->sources.request_hard_timeout_secs == config::Source::ProjectConfigFile);

    config::FileConfig global_file;
    global_file.request_hard_timeout_secs = 90;
    const auto from_global = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global_file,
                                                 EmptyGenericEnv());
    REQUIRE(from_global.has_value());
    CHECK(from_global->config.request_hard_timeout_secs == 90);
    CHECK(from_global->sources.request_hard_timeout_secs == config::Source::GlobalConfigFile);

    // 项目级 0 压全局 90:显式"不设墙"胜过全局的墙,不是被全局顶掉。
    const auto zero_over_global = config::MergeConfig(EmptyLubancodeEnv(), file, global_file,
                                                      EmptyGenericEnv());
    REQUIRE(zero_over_global.has_value());
    CHECK(zero_over_global->config.request_hard_timeout_secs == 600);  // file 是 600
    config::FileConfig zero_file;
    zero_file.request_hard_timeout_secs = 0;
    const auto zero_wins = config::MergeConfig(EmptyLubancodeEnv(), zero_file, global_file, EmptyGenericEnv());
    REQUIRE(zero_wins.has_value());
    CHECK(zero_wins->config.request_hard_timeout_secs == 0);
    CHECK(zero_wins->sources.request_hard_timeout_secs == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: 三个超时字段配置文件压过默认值,没写走内置默认") {
    const auto defaulted = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(defaulted.has_value());
    CHECK(defaulted->config.connect_timeout_ms == config::kDefaultConnectTimeoutMs);
    CHECK(defaulted->config.stream_idle_timeout_secs == config::kDefaultStreamIdleTimeoutSecs);
    CHECK(defaulted->config.request_timeout_secs == config::kDefaultRequestTimeoutSecs);
    CHECK(defaulted->sources.connect_timeout_ms == config::Source::Default);
    CHECK(defaulted->sources.stream_idle_timeout_secs == config::Source::Default);
    CHECK(defaulted->sources.request_timeout_secs == config::Source::Default);

    config::FileConfig file;
    file.connect_timeout_ms = 8000;
    file.stream_idle_timeout_secs = 45;
    file.request_timeout_secs = 20;
    const auto from_file = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(from_file.has_value());
    CHECK(from_file->config.connect_timeout_ms == 8000);
    CHECK(from_file->config.stream_idle_timeout_secs == 45);
    CHECK(from_file->config.request_timeout_secs == 20);
    CHECK(from_file->sources.connect_timeout_ms == config::Source::ProjectConfigFile);
    CHECK(from_file->sources.stream_idle_timeout_secs == config::Source::ProjectConfigFile);
    CHECK(from_file->sources.request_timeout_secs == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig: 三个超时字段项目级缺时回退全局") {
    config::FileConfig global_file;
    global_file.connect_timeout_ms = 9000;

    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global_file, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    CHECK(merged->config.connect_timeout_ms == 9000);
    CHECK(merged->sources.connect_timeout_ms == config::Source::GlobalConfigFile);
    // 项目级、全局都没写的另外两个字段,回退到内置默认值。
    CHECK(merged->config.stream_idle_timeout_secs == config::kDefaultStreamIdleTimeoutSecs);
    CHECK(merged->config.request_timeout_secs == config::kDefaultRequestTimeoutSecs);
}

// ---------------------------------------------------------------------------
// 分层合并(项目级 + 全局 config.json,逐字段):项目级压全局,项目级缺的
// 字段回退全局,两级都无回退 env/默认。来源细分成 ProjectConfigFile /
// GlobalConfigFile。
// ---------------------------------------------------------------------------

TEST_CASE("MergeConfig 分层: 项目级压过全局(同一字段两级都写)") {
    config::FileConfig project;
    project.theme = "light";
    project.source_path = "/proj/.lubancode/config.json";
    config::FileConfig global;
    global.theme = "dark";
    global.source_path = "/home/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.theme == "light");
    CHECK(result->sources.theme == config::Source::ProjectConfigFile);
}

TEST_CASE("MergeConfig 分层: 项目级缺的字段回退全局") {
    // 项目级只写 model,全局写 theme + base_url;各归各的来源。
    config::FileConfig project;
    project.model = "proj-model";
    project.source_path = "/proj/.lubancode/config.json";
    config::FileConfig global;
    global.theme = "light";
    global.base_url = "https://global.example.com";
    global.source_path = "/home/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(result.has_value());

    CHECK(result->config.model == "proj-model");
    CHECK(result->sources.model == config::Source::ProjectConfigFile);

    CHECK(result->config.theme == "light");
    CHECK(result->sources.theme == config::Source::GlobalConfigFile);

    CHECK(result->config.base_url == "https://global.example.com");
    CHECK(result->sources.base_url == config::Source::GlobalConfigFile);
}

TEST_CASE("MergeConfig 分层: 只有全局有这份文件,字段来源记全局") {
    config::FileConfig global;
    global.theme = "plain";
    global.max_context_chars = 4242;
    global.source_path = "/home/.lubancode/config.json";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.theme == "plain");
    CHECK(result->sources.theme == config::Source::GlobalConfigFile);
    CHECK(result->config.max_context_chars == 4242);
    CHECK(result->sources.max_context_chars == config::Source::GlobalConfigFile);
}

TEST_CASE("MergeConfig 分层: 两级都没这字段,回退 env / 默认") {
    config::FileConfig project;
    project.model = "proj-model";  // 只写 model
    project.source_path = "/proj/.lubancode/config.json";
    config::FileConfig global;
    global.source_path = "/home/.lubancode/config.json";  // 空全局

    config::GenericEnvValues generic;
    generic.anthropic_base_url = "https://generic.example.com";

    const auto result = config::MergeConfig(EmptyLubancodeEnv(), project, global, generic);
    REQUIRE(result.has_value());
    // base_url 两级配置文件都没有,落到通用 env。
    CHECK(result->config.base_url == "https://generic.example.com");
    CHECK(result->sources.base_url == config::Source::GenericEnv);
    // theme 两级都没有,落到内置默认。
    CHECK(result->config.theme == config::kDefaultTheme);
    CHECK(result->sources.theme == config::Source::Default);
}

TEST_CASE("MergeConfig 分层: 专属 env 压过项目级与全局") {
    config::LubancodeEnvValues env;
    env.theme = "plain";
    config::FileConfig project;
    project.theme = "light";
    project.source_path = "/proj/.lubancode/config.json";
    config::FileConfig global;
    global.theme = "dark";
    global.source_path = "/home/.lubancode/config.json";

    const auto result = config::MergeConfig(env, project, global, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.theme == "plain");
    CHECK(result->sources.theme == config::Source::LubancodeEnv);
}

TEST_CASE("MergeConfig 分层: 对象整段(search)项目级压全局,项目级没有才回退全局") {
    config::SearchConfig proj_search;
    proj_search.provider = "tavily";
    proj_search.api_key = "proj-key";
    config::SearchConfig glob_search;
    glob_search.provider = "brave";
    glob_search.api_key = "glob-key";

    // 两级都写:用项目级那一整段。
    config::FileConfig project;
    project.search = proj_search;
    project.source_path = "/proj/.lubancode/config.json";
    config::FileConfig global;
    global.search = glob_search;
    global.source_path = "/home/.lubancode/config.json";
    const auto both = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(both.has_value());
    CHECK(both->config.search.provider == "tavily");
    CHECK(both->config.search.api_key == "proj-key");

    // 只有全局写:回退全局那一整段。
    config::FileConfig project_empty;
    project_empty.source_path = "/proj/.lubancode/config.json";
    const auto only_global = config::MergeConfig(EmptyLubancodeEnv(), project_empty, global, EmptyGenericEnv());
    REQUIRE(only_global.has_value());
    CHECK(only_global->config.search.provider == "brave");
    CHECK(only_global->config.search.api_key == "glob-key");
}

TEST_CASE("MergeConfig 分层: wire 坏值报错时,带上写了这个坏值的那一级文件路径") {
    config::FileConfig global;
    global.wire = "bogus";
    global.source_path = "/home/.lubancode/config.json";
    // 项目级没写 wire,坏值来自全局——报错要指向全局那份文件。
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global, EmptyGenericEnv());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("/home/.lubancode/config.json") != std::string::npos);
    CHECK(result.error().find("bogus") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ParseSettingsLocal:纯函数,项目级本地权限。完整/部分/坏 JSON/空/没有
// permissions 段各测一遍。
// ---------------------------------------------------------------------------

TEST_CASE("ParseSettingsLocal: 完整字段都解出来") {
    const std::string json = R"({
        "permissions": {
            "allow_tools": ["write_file", "run_command"],
            "allow_commands": ["npm test", "git status"],
            "deny_commands": ["rm -rf"],
            "default_confirm_mode": "auto"
        }
    })";
    const auto result = config::ParseSettingsLocal(json, "/p/settings.local.json");
    REQUIRE(result.has_value());
    REQUIRE(result->allow_tools.size() == 2);
    CHECK(result->allow_tools[0] == "write_file");
    CHECK(result->allow_tools[1] == "run_command");
    REQUIRE(result->allow_commands.size() == 2);
    CHECK(result->allow_commands[0] == "npm test");
    REQUIRE(result->deny_commands.size() == 1);
    CHECK(result->deny_commands[0] == "rm -rf");
    REQUIRE(result->default_confirm_mode.has_value());
    CHECK(*result->default_confirm_mode == "auto");
    CHECK_FALSE(result->Empty());
}

TEST_CASE("ParseSettingsLocal: 部分字段,缺的留空/nullopt") {
    const auto result = config::ParseSettingsLocal(
        R"({"permissions": {"allow_tools": ["write_file"]}})", "/p/settings.local.json");
    REQUIRE(result.has_value());
    REQUIRE(result->allow_tools.size() == 1);
    CHECK(result->allow_commands.empty());
    CHECK(result->deny_commands.empty());
    CHECK_FALSE(result->default_confirm_mode.has_value());
}

TEST_CASE("ParseSettingsLocal: 没有 permissions 段,返回空 SettingsLocal(不算错)") {
    const auto result = config::ParseSettingsLocal(R"({"其他字段": 1})", "/p/settings.local.json");
    REQUIRE(result.has_value());
    CHECK(result->Empty());
}

TEST_CASE("ParseSettingsLocal: 空 object 就是空 SettingsLocal") {
    const auto result = config::ParseSettingsLocal("{}", "/p/settings.local.json");
    REQUIRE(result.has_value());
    CHECK(result->Empty());
}

TEST_CASE("ParseSettingsLocal: 坏 JSON 报错,带上路径") {
    const auto result = config::ParseSettingsLocal("{ not json ", "/home/u/settings.local.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("/home/u/settings.local.json") != std::string::npos);
}

TEST_CASE("ParseSettingsLocal: 顶层不是 object 报错") {
    const auto result = config::ParseSettingsLocal("[1,2,3]", "/p/settings.local.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseSettingsLocal: permissions 不是 object 报错;数组里混进非字符串元素跳过") {
    CHECK_FALSE(config::ParseSettingsLocal(R"({"permissions": 42})", "p").has_value());

    const auto mixed = config::ParseSettingsLocal(
        R"({"permissions": {"allow_tools": ["ok", 5, "also_ok"]}})", "p");
    REQUIRE(mixed.has_value());
    REQUIRE(mixed->allow_tools.size() == 2);
    CHECK(mixed->allow_tools[0] == "ok");
    CHECK(mixed->allow_tools[1] == "also_ok");
}

TEST_CASE("ParseSettingsLocal: default_confirm_mode 空串当没设,别的字符串原样留着") {
    const auto empty_mode = config::ParseSettingsLocal(
        R"({"permissions": {"default_confirm_mode": ""}})", "p");
    REQUIRE(empty_mode.has_value());
    CHECK_FALSE(empty_mode->default_confirm_mode.has_value());

    const auto weird = config::ParseSettingsLocal(
        R"({"permissions": {"default_confirm_mode": "wat"}})", "p");
    REQUIRE(weird.has_value());
    REQUIRE(weird->default_confirm_mode.has_value());
    CHECK(*weird->default_confirm_mode == "wat");  // 认不认得交给调用方判
}

// ---------------------------------------------------------------------------
// ClassifyCommandByPermissions:deny 压 allow、allow 命中、都没命中,前缀
// 判定去前导空白。这是 main 的 auto 分流叠加判定的纯函数内核。
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyCommandByPermissions: allow 前缀命中 → Allow") {
    const std::vector<std::string> allow = {"npm test", "git status"};
    const std::vector<std::string> deny;
    CHECK(config::ClassifyCommandByPermissions("npm test --watch", allow, deny) ==
          config::CommandPermission::Allow);
    CHECK(config::ClassifyCommandByPermissions("git status", allow, deny) ==
          config::CommandPermission::Allow);
}

TEST_CASE("ClassifyCommandByPermissions: deny 前缀命中 → Deny") {
    const std::vector<std::string> allow;
    const std::vector<std::string> deny = {"rm -rf"};
    CHECK(config::ClassifyCommandByPermissions("rm -rf /tmp/x", allow, deny) ==
          config::CommandPermission::Deny);
}

TEST_CASE("ClassifyCommandByPermissions: deny 压过 allow(两边都命中算 Deny)") {
    const std::vector<std::string> allow = {"git"};
    const std::vector<std::string> deny = {"git push"};
    CHECK(config::ClassifyCommandByPermissions("git push origin main", allow, deny) ==
          config::CommandPermission::Deny);
}

TEST_CASE("ClassifyCommandByPermissions: 都没命中 → None;前缀判定去前导空白") {
    const std::vector<std::string> allow = {"npm test"};
    const std::vector<std::string> deny = {"rm -rf"};
    CHECK(config::ClassifyCommandByPermissions("echo hi", allow, deny) == config::CommandPermission::None);
    // 前导空白不影响命中。
    CHECK(config::ClassifyCommandByPermissions("   npm test", allow, deny) ==
          config::CommandPermission::Allow);
    // 空名单恒不命中。
    CHECK(config::ClassifyCommandByPermissions("npm test", {}, {}) == config::CommandPermission::None);
}

// ---------------------------------------------------------------------------
// AddAllowedToolToSettingsLocal + LoadSettingsLocal + EnsureGitignore:真在
// 临时 cwd 目录里读写(用完删),按需建 .lubancode/、去重、保留别的字段。
// ---------------------------------------------------------------------------

namespace {

class TempCwdDir {
public:
    TempCwdDir() {
        dir_ = std::filesystem::temp_directory_path() /
               ("lubancode_settings_local_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }
    ~TempCwdDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string Path() const { return dir_.string(); }
    std::string ReadFile(const std::string& rel) const {
        std::ifstream in(dir_ / std::filesystem::path(rel), std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    void WriteFile(const std::string& rel, const std::string& content) const {
        const auto path = dir_ / std::filesystem::path(rel);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

private:
    std::filesystem::path dir_;
};

}  // namespace

TEST_CASE("AddAllowedToolToSettingsLocal: 文件/目录不存在时按需创建,LoadSettingsLocal 读得回来") {
    TempCwdDir cwd;
    // 一开始没有 .lubancode/,LoadSettingsLocal 返回 nullopt(不算错)。
    const auto before = config::LoadSettingsLocal(cwd.Path());
    REQUIRE(before.has_value());
    CHECK_FALSE(before->has_value());

    const auto written = config::AddAllowedToolToSettingsLocal(cwd.Path(), "write_file");
    REQUIRE(written.has_value());
    CHECK(std::filesystem::exists(*written));

    const auto loaded = config::LoadSettingsLocal(cwd.Path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->allow_tools.size() == 1);
    CHECK((*loaded)->allow_tools[0] == "write_file");
}

TEST_CASE("AddAllowedToolToSettingsLocal: 幂等去重,保留别的字段") {
    TempCwdDir cwd;
    cwd.WriteFile(".lubancode/settings.local.json",
                  R"({"permissions": {"allow_tools": ["write_file"], "deny_commands": ["rm -rf"]}, "自定义": 7})");

    // 已在的工具不重复写。
    REQUIRE(config::AddAllowedToolToSettingsLocal(cwd.Path(), "write_file").has_value());
    // 新工具追加。
    REQUIRE(config::AddAllowedToolToSettingsLocal(cwd.Path(), "run_command").has_value());

    const auto loaded = config::LoadSettingsLocal(cwd.Path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->allow_tools.size() == 2);
    CHECK((*loaded)->deny_commands.size() == 1);  // 别的字段没被冲掉

    const std::string raw = cwd.ReadFile(".lubancode/settings.local.json");
    CHECK(raw.find("自定义") != std::string::npos);  // 不认得的顶层字段也保留
}

TEST_CASE("EnsureGitignoreCoversSettingsLocal: 有 .gitignore 没挡就追加;已挡住不动;没有则给提示") {
    {
        TempCwdDir cwd;
        cwd.WriteFile(".gitignore", "build/\n");
        const std::string notice = config::EnsureGitignoreCoversSettingsLocal(cwd.Path());
        CHECK(notice.find(".gitignore") != std::string::npos);
        CHECK(cwd.ReadFile(".gitignore").find(".lubancode/settings.local.json") != std::string::npos);
    }
    {
        TempCwdDir cwd;
        cwd.WriteFile(".gitignore", ".lubancode/\n");  // 整个目录已忽略
        const std::string notice = config::EnsureGitignoreCoversSettingsLocal(cwd.Path());
        CHECK(notice.empty());  // 已挡住,什么都不做
    }
    {
        TempCwdDir cwd;  // 没有 .gitignore
        const std::string notice = config::EnsureGitignoreCoversSettingsLocal(cwd.Path());
        CHECK(notice.find("提示") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// providers:配置解析/整段合并/回写。密钥只存环境变量名，绝不应出现在 JSON。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: providers 解出默认 key_env 与上下文窗口") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"minimax","base_url":"https://api.minimax.io/anthropic","wire":"anthropic","model":"MiniMax-M2","context_window":"1m"}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    REQUIRE(parsed->providers->size() == 1);
    const auto& provider = parsed->providers->front();
    CHECK(provider.name == "minimax");
    CHECK(provider.wire == config::Wire::Anthropic);
    CHECK(provider.key_env == "ANTHROPIC_AUTH_TOKEN");
    CHECK(provider.model == "MiniMax-M2");
    CHECK(provider.context_window_tokens == 1000000);
}

TEST_CASE("ParseProviderWire: 规范名四枚齐认,ProviderWireName 一律吐规范名") {
    CHECK(*config::ParseProviderWire("anthropic-messages") == config::Wire::Anthropic);
    CHECK(*config::ParseProviderWire("openai-responses") == config::Wire::Responses);
    CHECK(*config::ParseProviderWire("openai-chat-completions") == config::Wire::ChatCompletions);
    CHECK(*config::ParseProviderWire("google-generate-content") == config::Wire::GoogleGenerateContent);

    CHECK(config::ProviderWireName(config::Wire::Anthropic) == "anthropic-messages");
    CHECK(config::ProviderWireName(config::Wire::Responses) == "openai-responses");
    CHECK(config::ProviderWireName(config::Wire::ChatCompletions) == "openai-chat-completions");
    CHECK(config::ProviderWireName(config::Wire::GoogleGenerateContent) == "google-generate-content");

    // 认不得的值报错,错误信息把四枚规范名列全。
    const auto bad = config::ParseProviderWire("grpc");
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().find("google-generate-content") != std::string::npos);
}

TEST_CASE("ParseProviderWire: 旧名(anthropic/responses/chat_completions/chat)永久当别名认") {
    CHECK(*config::ParseProviderWire("anthropic") == config::Wire::Anthropic);
    CHECK(*config::ParseProviderWire("responses") == config::Wire::Responses);
    CHECK(*config::ParseProviderWire("chat_completions") == config::Wire::ChatCompletions);
    CHECK(*config::ParseProviderWire("chat") == config::Wire::ChatCompletions);
}

TEST_CASE("wire 更名迁移: 旧名配置读入→保存→再读,文件里已升成新规范名") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    // 一份旧写法的配置:四家 wire 三旧一新,加载与保存链路都要各归各位。
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[)"
                                             R"({"name":"legacy-chat","base_url":"https://a.test","wire":"chat_completions"},)"
                                             R"({"name":"legacy-alias","base_url":"https://a2.test","wire":"chat"},)"
                                             R"({"name":"legacy-responses","base_url":"https://b.test","wire":"responses"},)"
                                             R"({"name":"legacy-anthropic","base_url":"https://c.test","wire":"anthropic"},)"
                                             R"({"name":"fresh-gemini","base_url":"https://d.test","wire":"google-generate-content"}]})");

    // 1) 加载:旧名/别名/新名全部解析成同一批枚举,语义不变。
    const auto loaded = config::ParseProvidersConfig(
        nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"))["providers"], path.string());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 5);
    CHECK((*loaded)[0].wire == config::Wire::ChatCompletions);
    CHECK((*loaded)[1].wire == config::Wire::ChatCompletions);
    CHECK((*loaded)[2].wire == config::Wire::Responses);
    CHECK((*loaded)[3].wire == config::Wire::Anthropic);
    CHECK((*loaded)[4].wire == config::Wire::GoogleGenerateContent);

    // 2) 保存(回写走 ProvidersToJson,wire 一律 ProviderWireName):文件里
    //    的旧名就地升成新名,迁移静默完成。
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), *loaded).has_value());
    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK(written["providers"][0]["wire"] == "openai-chat-completions");
    CHECK(written["providers"][1]["wire"] == "openai-chat-completions");
    CHECK(written["providers"][2]["wire"] == "openai-responses");
    CHECK(written["providers"][3]["wire"] == "anthropic-messages");
    CHECK(written["providers"][4]["wire"] == "google-generate-content");

    // 3) 再读:新名解析回同一批枚举,来回一个来回语义没漂。
    const auto reloaded = config::ParseProvidersConfig(written["providers"], path.string());
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->size() == loaded->size());
    for (std::size_t i = 0; i < loaded->size(); ++i) {
        CHECK((*reloaded)[i].name == (*loaded)[i].name);
        CHECK((*reloaded)[i].wire == (*loaded)[i].wire);
    }
}

TEST_CASE("wire 更名迁移: 顶层单 provider 旧写法经 MergeConfig 归一,落盘路径只认规范名") {
    config::FileConfig file;
    file.source_path = "/tmp/config.json";
    file.wire = "chat_completions";
    file.base_url = "https://a.test";
    file.api_key = "sk-x";
    file.model = "m";

    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), file, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    CHECK(merged->config.wire == config::Wire::ChatCompletions);
    // SaveConfigFile 的写盘值出自 ProviderWireName(枚举唯一出口),顶层
    // 旧写法同样在第一次保存时就地升名。
    CHECK(config::ProviderWireName(merged->config.wire) == "openai-chat-completions");
}

TEST_CASE("ParseFileConfigJson: providers 坏地址、坏协议与重名都拦下") {
    const auto bad_url = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"api.example.test","wire":"anthropic"}]})", "/tmp/config.json");
    CHECK_FALSE(bad_url.has_value());
    CHECK(bad_url.error().find("http") != std::string::npos);

    const auto bad_wire = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://api.example.test","wire":"unknown"}]})", "/tmp/config.json");
    CHECK_FALSE(bad_wire.has_value());
    CHECK(bad_wire.error().find("wire") != std::string::npos);

    const auto duplicate = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic"},{"name":"x","base_url":"https://b.test","wire":"responses"}]})",
        "/tmp/config.json");
    CHECK_FALSE(duplicate.has_value());
    CHECK(duplicate.error().find("重复") != std::string::npos);
}

TEST_CASE("MergeConfig: 项目级 providers 整段压过全局") {
    config::FileConfig project;
    project.source_path = "/project/config.json";
    project.providers = std::vector<config::ProviderConfig>{{
        .name = "project",
        .base_url = "https://project.example.test",
        .wire = config::Wire::Responses,
        .model = "project-model",
    }};
    config::FileConfig global;
    global.source_path = "/global/config.json";
    global.providers = std::vector<config::ProviderConfig>{{
        .name = "global",
        .base_url = "https://global.example.test",
        .wire = config::Wire::Anthropic,
        .model = "global-model",
    }};

    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    REQUIRE(merged->config.providers.size() == 1);
    CHECK(merged->config.providers.front().name == "project");
    CHECK(merged->sources.providers == config::Source::ProjectConfigFile);
}

TEST_CASE("provider runtime: 明选一端便整套覆盖旧连接与能力字段") {
    config::Config current;
    current.base_url = "https://stale.example/v1";
    current.model = "stale-model";
    current.auth_token = "stale-key";

    config::ProviderConfig provider;
    provider.name = "fresh";
    provider.base_url = "https://fresh.example/v1";
    provider.wire = config::Wire::Responses;
    provider.auth = config::ProviderAuthMode::Inline;
    provider.api_key = "fresh-key";
    provider.model = "fresh-model";
    provider.context_window_tokens = 131072;
    provider.native_web_search = true;
    provider.stream_usage = true;
    provider.stream_usage_declared = true;
    provider.reasoning_replay = "tool_episode";
    provider.reasoning_delta_field = "reasoning_content";
    provider.reasoning_replay_field = "reasoning";
    provider.supported_think_levels = {"low", "high"};
    provider.think_param = "effort";
    provider.think_passthrough = true;
    provider.metrics_url = "http://127.0.0.1:9000/metrics";
    provider.max_output_tokens = 8192;
    provider.extra_body = nlohmann::json{{"reasoning", true}};
    provider.extra_headers = {{"X-Api-Version", "2026-08-25"}};

    config::ApplyProviderToRuntimeConfig(current, provider);

    CHECK(current.active_provider == "fresh");
    CHECK(current.base_url == provider.base_url);
    CHECK(current.wire == provider.wire);
    CHECK(current.auth_token == "fresh-key");
    CHECK(current.model == "fresh-model");
    CHECK(current.context_window_tokens == 131072);
    CHECK(current.reasoning_delta_field == "reasoning_content");
    CHECK(current.reasoning_replay_field == "reasoning");
    CHECK(current.provider_max_output_tokens == 8192);
    CHECK(current.extra_body == provider.extra_body);
    CHECK(current.extra_headers == provider.extra_headers);
}

TEST_CASE("active_provider: 解析、分层并展开对应 provider") {
    const auto parsed = config::ParseFileConfigJson(R"({"active_provider":"sub-openai"})", "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->active_provider.has_value());
    CHECK(*parsed->active_provider == "sub-openai");

    const auto bad = config::ParseFileConfigJson(R"({"active_provider":7})", "/tmp/config.json");
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().find("active_provider") != std::string::npos);

    config::FileConfig global;
    global.source_path = "/global/config.json";
    global.active_provider = "sub-openai";
    global.providers = std::vector<config::ProviderConfig>{{
        .name = "sub-openai",
        .base_url = "https://api.example.test",
        .wire = config::Wire::Responses,
        .api_key = "sk-provider",
        .model = "gpt-test",
        .model_reasoning_effort = "xhigh",
        .context_window_tokens = 512000,
        .native_web_search = true,
        .extra_body = nlohmann::json{{"reasoning", "high"}},
        .extra_headers = {{"X-Test", "yes"}},
    }};
    auto merged = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, global, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    CHECK(merged->config.active_provider == "sub-openai");
    CHECK(merged->sources.active_provider == config::Source::GlobalConfigFile);
    REQUIRE(config::ApplyConfiguredActiveProvider(*merged));
    CHECK(merged->config.wire == config::Wire::Responses);
    CHECK(merged->config.base_url == "https://api.example.test");
    CHECK(merged->config.auth_token == "sk-provider");
    CHECK(merged->config.model == "gpt-test");
    CHECK(merged->config.think == "xhigh");
    CHECK(merged->config.context_window_tokens == 512000);
    CHECK(merged->config.native_web_search);
    CHECK(merged->config.extra_body.at("reasoning") == "high");
    CHECK(merged->config.extra_headers.at("X-Test") == "yes");
}

TEST_CASE("active_provider: LUBANCODE 专属环境变量仍压过 provider,旧名字不拦启动") {
    config::FileConfig global;
    global.source_path = "/global/config.json";
    global.active_provider = "sub";
    global.think = "medium";  // 顶层明配，不算 provider 私货，须照旧保留。
    global.providers = std::vector<config::ProviderConfig>{{
        .name = "sub",
        .base_url = "https://provider.test",
        .wire = config::Wire::Responses,
        .api_key = "provider-key",
        .model = "provider-model",
        .model_reasoning_effort = "xhigh",
        .context_window_tokens = 12345,
        .native_web_search = true,
        .stream_usage = true,
        .reasoning_replay = "tool_episode",
        .supported_think_levels = {"low", "xhigh"},
        .metrics_url = "https://provider.test/metrics",
    }};
    config::LubancodeEnvValues env;
    env.base_url = "https://env.test";
    env.api_key = "env-key";
    env.model = "env-model";
    auto merged = config::MergeConfig(env, std::nullopt, global, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    REQUIRE(config::ApplyConfiguredActiveProvider(*merged));
    CHECK(merged->config.base_url == "https://env.test");
    CHECK(merged->config.auth_token == "env-key");
    CHECK(merged->config.model == "env-model");
    CHECK(merged->config.context_window_tokens != 12345);
    CHECK(merged->config.think == "medium");
    CHECK_FALSE(merged->config.native_web_search);
    CHECK_FALSE(merged->config.stream_usage);
    CHECK(merged->config.reasoning_replay.empty());
    CHECK(merged->config.provider_think_levels.empty());
    CHECK(merged->config.metrics_url.empty());
    CHECK(config::EnvironmentOverridesActiveProvider(merged->config, merged->sources, "sub"));

    merged->config.active_provider = "removed";
    CHECK_FALSE(config::ApplyConfiguredActiveProvider(*merged));
    CHECK(merged->config.active_provider.empty());
    CHECK(merged->sources.active_provider == config::Source::Default);
}

TEST_CASE("active_provider: 项目级选择可钉住全局 providers") {
    config::FileConfig project;
    project.source_path = "/project/config.json";
    project.active_provider = "work";
    project.base_url = "https://stale-project-value.test";
    config::FileConfig global;
    global.source_path = "/global/config.json";
    global.providers = std::vector<config::ProviderConfig>{{
        .name = "work",
        .base_url = "https://selected.test",
        .wire = config::Wire::Responses,
        .api_key = "key",
        .model = "selected-model",
    }};

    auto merged = config::MergeConfig(EmptyLubancodeEnv(), project, global, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    REQUIRE(config::ApplyConfiguredActiveProvider(*merged));
    CHECK(merged->config.base_url == "https://selected.test");
    CHECK(merged->sources.base_url == config::Source::ProjectConfigFile);
    CHECK(merged->sources.active_provider == config::Source::ProjectConfigFile);
}

TEST_CASE("UpdateActiveProviderInConfigFile: 只改选择名,其余字段原样保留") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"theme":"plain","自定义":7,"providers":[]})");

    REQUIRE(config::UpdateActiveProviderInConfigFile(path.string(), "sub-openai").has_value());
    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK(written["active_provider"] == "sub-openai");
    CHECK(written["theme"] == "plain");
    CHECK(written["自定义"] == 7);
    CHECK(written["providers"].empty());
}

TEST_CASE("UpdateProvidersInConfigFile: 回写保留旁字段且不落明文密钥") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"theme":"plain","自定义":7,"providers":[]})");

    const std::vector<config::ProviderConfig> providers{{
        .name = "glm",
        .base_url = "https://open.bigmodel.cn/api/paas/v4",
        .wire = config::Wire::Responses,
        .key_env = "ZAI_API_KEY",
        .model = "glm-4.5",
        .context_window_tokens = 128000,
    }};
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());

    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK(written["theme"] == "plain");
    CHECK(written["自定义"] == 7);
    REQUIRE(written["providers"].size() == 1);
    CHECK(written["providers"][0]["key_env"] == "ZAI_API_KEY");
    CHECK_FALSE(written["providers"][0].contains("api_key"));
}

// ---------------------------------------------------------------------------
// providers 新字段:api_key(可选,直接贴 key 落盘)、model_reasoning_effort
// (可选,切端时应用)。#59 /provider add 向导新增。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: providers 解出 api_key 与 model_reasoning_effort") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"sub-openai","base_url":"https://cc.moontidef.work","wire":"responses",)"
        R"("api_key":"sk-pasted-key","model":"gpt-5.5","model_reasoning_effort":"xhigh"}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    REQUIRE(parsed->providers->size() == 1);
    const auto& provider = parsed->providers->front();
    CHECK(provider.api_key == "sk-pasted-key");
    CHECK(provider.model_reasoning_effort == "xhigh");
}

TEST_CASE("ParseFileConfigJson: providers 没写 api_key/model_reasoning_effort 时两个字段都留空") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"minimax","base_url":"https://api.minimax.io/anthropic","wire":"anthropic"}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    REQUIRE(parsed->providers->size() == 1);
    const auto& provider = parsed->providers->front();
    CHECK(provider.api_key.empty());
    CHECK(provider.model_reasoning_effort.empty());
}

TEST_CASE("ParseFileConfigJson: providers 的 api_key/model_reasoning_effort 类型不对就报错") {
    const auto bad_api_key = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","api_key":7}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_api_key.has_value());
    CHECK(bad_api_key.error().find("api_key") != std::string::npos);

    const auto bad_effort = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","model_reasoning_effort":7}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_effort.has_value());
    CHECK(bad_effort.error().find("model_reasoning_effort") != std::string::npos);
}

TEST_CASE("UpdateProvidersInConfigFile: api_key/model_reasoning_effort 设置了就落盘,回读原样") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[]})");

    const std::vector<config::ProviderConfig> providers{{
        .name = "sub-openai",
        .base_url = "https://cc.moontidef.work",
        .wire = config::Wire::Responses,
        .api_key = "sk-pasted-key",
        .model = "gpt-5.5",
        .model_reasoning_effort = "xhigh",
    }};
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());

    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    REQUIRE(written["providers"].size() == 1);
    CHECK(written["providers"][0]["api_key"] == "sk-pasted-key");
    CHECK(written["providers"][0]["model_reasoning_effort"] == "xhigh");

    // 回读能解析出同样的值(往返不丢字段)。
    const auto reparsed = config::ParseFileConfigJson(written.dump(), path.string());
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->providers.has_value());
    REQUIRE(reparsed->providers->size() == 1);
    CHECK(reparsed->providers->front().api_key == "sk-pasted-key");
    CHECK(reparsed->providers->front().model_reasoning_effort == "xhigh");
}

// ---------------------------------------------------------------------------
// providers 新字段:native_web_search(可选,默认 false)。M12 原生 web_search
// 声明,每个 provider 各自开关。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: providers 没写 native_web_search 时默认 false,旧配置读进来不报错") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"minimax","base_url":"https://api.minimax.io/anthropic","wire":"anthropic"}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    REQUIRE(parsed->providers->size() == 1);
    CHECK_FALSE(parsed->providers->front().native_web_search);
}

TEST_CASE("ParseFileConfigJson: providers 写 native_web_search:true 时解出 true") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"sub-openai","base_url":"https://cc.moontidef.work","wire":"responses",)"
        R"("model":"gpt-5.5","native_web_search":true}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    REQUIRE(parsed->providers->size() == 1);
    CHECK(parsed->providers->front().native_web_search);
}

TEST_CASE("ParseFileConfigJson: providers 的 native_web_search 类型不对就报错") {
    const auto bad = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","native_web_search":"yes"}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().find("native_web_search") != std::string::npos);
}

TEST_CASE("UpdateProvidersInConfigFile: native_web_search=true 落盘,回读原样;=false 不落这个键") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[]})");

    const std::vector<config::ProviderConfig> providers{
        {
            .name = "sub-openai",
            .base_url = "https://cc.moontidef.work",
            .wire = config::Wire::Responses,
            .model = "gpt-5.5",
            .native_web_search = true,
        },
        {
            .name = "minimax",
            .base_url = "https://api.minimax.io/anthropic",
            .wire = config::Wire::Anthropic,
            .model = "MiniMax-M2",
            .native_web_search = false,
        },
    };
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());

    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    REQUIRE(written["providers"].size() == 2);
    CHECK(written["providers"][0]["native_web_search"] == true);
    CHECK_FALSE(written["providers"][1].contains("native_web_search"));

    // 回读能解析出同样的值(往返不丢字段)。
    const auto reparsed = config::ParseFileConfigJson(written.dump(), path.string());
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->providers.has_value());
    REQUIRE(reparsed->providers->size() == 2);
    CHECK(reparsed->providers->at(0).native_web_search);
    CHECK_FALSE(reparsed->providers->at(1).native_web_search);
}

// ---------------------------------------------------------------------------
// /provider set native_web_search on|off:ParseBoolToggle 解开关值,
// SetProviderNativeWebSearch 在内存里改字段,两个凑一块再落盘就是
// HandleProviderCommand 的 Set 分支实际干的事。
// ---------------------------------------------------------------------------

TEST_CASE("ParseBoolToggle: on/off、true/false、1/0 都认,大小写不敏感") {
    for (const char* const truthy : {"on", "On", "ON", "true", "True", "1"}) {
        const auto parsed = config::ParseBoolToggle(truthy);
        REQUIRE(parsed.has_value());
        CHECK(*parsed);
    }
    for (const char* const falsy : {"off", "Off", "OFF", "false", "False", "0"}) {
        const auto parsed = config::ParseBoolToggle(falsy);
        REQUIRE(parsed.has_value());
        CHECK_FALSE(*parsed);
    }
}

TEST_CASE("ParseBoolToggle: 认不出的写法报错,不崩") {
    const auto parsed = config::ParseBoolToggle("yes");
    CHECK_FALSE(parsed.has_value());
    CHECK(parsed.error().find("yes") != std::string::npos);
}

TEST_CASE("SetProviderNativeWebSearch: 找得到就地改字段,返回 true") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://open.bigmodel.cn/api/paas/v4", .wire = config::Wire::Responses},
        {.name = "minimax", .base_url = "https://api.minimax.io/anthropic", .wire = config::Wire::Anthropic},
    };
    CHECK(config::SetProviderNativeWebSearch(providers, "glm", true));
    CHECK(providers[0].native_web_search);
    CHECK_FALSE(providers[1].native_web_search);  // 没点名的那条不受影响

    CHECK(config::SetProviderNativeWebSearch(providers, "glm", false));
    CHECK_FALSE(providers[0].native_web_search);
}

TEST_CASE("SetProviderNativeWebSearch: 名字不存在返回 false,列表原样不动") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://open.bigmodel.cn/api/paas/v4", .wire = config::Wire::Responses},
    };
    CHECK_FALSE(config::SetProviderNativeWebSearch(providers, "no-such-provider", true));
    // 没找到就没改;列表大小、字段都原样,证明不会误伤别的条目。
    REQUIRE(providers.size() == 1);
    CHECK_FALSE(providers[0].native_web_search);
}

// ---------------------------------------------------------------------------
// /provider edit(容错单):ReplaceProvider 纯函数整条替换,别的条目与改名
// 护栏都钉死。落盘(ReplaceProviderInGlobalConfig)与 Add/Set 家族同一条
// 路,管道抽验手工看。
// ---------------------------------------------------------------------------

TEST_CASE("ReplaceProvider: 整条换掉,别的条目原样,向导不碰的字段也带得回来") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://open.bigmodel.cn/api/paas/v4", .wire = config::Wire::Responses},
        {.name = "minimax", .base_url = "https://api.minimax.io/anthropic", .wire = config::Wire::Anthropic},
    };
    config::ProviderConfig edited = providers[0];
    edited.base_url = "https://new.example.test/v1";
    edited.context_window_tokens = 300000;  // 向导不编辑的字段,靠副本原样回写
    edited.supported_think_levels = {"low", "high"};

    CHECK(config::ReplaceProvider(providers, "glm", edited));
    REQUIRE(providers.size() == 2);
    CHECK(providers[0].base_url == "https://new.example.test/v1");
    CHECK(providers[0].context_window_tokens == 300000);
    CHECK(providers[0].supported_think_levels.size() == 2);
    CHECK(providers[0].name == "glm");  // 条目还叫 glm
    // 没点名的那条不受影响。
    CHECK(providers[1].base_url == "https://api.minimax.io/anthropic");
}

TEST_CASE("ReplaceProvider: 改名与找不到名字都返回 false,列表原样不动") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://a.test", .wire = config::Wire::Anthropic},
    };
    config::ProviderConfig renamed = providers[0];
    renamed.name = "glm2";
    CHECK_FALSE(config::ReplaceProvider(providers, "glm", renamed));  // 改名不留暗门

    config::ProviderConfig other;
    other.name = "other";
    CHECK_FALSE(config::ReplaceProvider(providers, "no-such-provider", other));

    REQUIRE(providers.size() == 1);
    CHECK(providers[0].name == "glm");
    CHECK(providers[0].base_url == "https://a.test");
}

TEST_CASE("/provider set 落盘路径:SetProviderNativeWebSearch 改完再 UpdateProvidersInConfigFile,临时文件回读原样") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[]})");

    std::vector<config::ProviderConfig> providers{
        {.name = "sub-openai", .base_url = "https://cc.moontidef.work", .wire = config::Wire::Responses,
         .model = "gpt-5.5"},
    };
    REQUIRE(config::SetProviderNativeWebSearch(providers, "sub-openai", true));
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());

    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    REQUIRE(written["providers"].size() == 1);
    CHECK(written["providers"][0]["native_web_search"] == true);

    // 再关一次,落盘应该原样把键去掉(native_web_search=false 不落盘,跟
    // UpdateProvidersInConfigFile 既有约定一致)。
    REQUIRE(config::SetProviderNativeWebSearch(providers, "sub-openai", false));
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());
    const nlohmann::json written_off = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK_FALSE(written_off["providers"][0].contains("native_web_search"));
}

TEST_CASE("/provider set 名字不存在:不该调 UpdateProvidersInConfigFile,配置文件原样不动") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    const std::string original = R"({"providers":[{"name":"glm","base_url":"https://a.test","wire":"anthropic"}]})";
    cwd.WriteFile(".lubancode/config.json", original);

    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://a.test", .wire = config::Wire::Anthropic},
    };
    // 跟 main.cpp HandleProviderCommand 的 Set 分支一样:SetProviderNativeWebSearch
    // 返回 false 就直接报错返回,不往下调 UpdateProvidersInConfigFile。
    const bool found = config::SetProviderNativeWebSearch(providers, "no-such-provider", true);
    CHECK_FALSE(found);
    if (found) {
        FAIL("不该走到这一步:名字没找到时不许落盘");
    }

    // 配置文件字节应该原样不动(没被误写)。
    CHECK(cwd.ReadFile(".lubancode/config.json") == original);
}

TEST_CASE("ProviderApiKey: api_key 非空时优先于 key_env,不管环境变量有没有设置") {
    config::ProviderConfig provider;
    provider.key_env = "LUBANCODE_TEST_PROVIDER_KEY_ENV_DOES_NOT_EXIST_XYZ";
    provider.api_key = "sk-direct-pasted-key";
    const auto key = config::ProviderApiKey(provider);
    REQUIRE(key.has_value());
    CHECK(*key == "sk-direct-pasted-key");
}

TEST_CASE("ProviderApiKey: api_key 为空时退回 key_env 环境变量,没设置就是 nullopt") {
    config::ProviderConfig provider;
    provider.key_env = "LUBANCODE_TEST_PROVIDER_KEY_ENV_DOES_NOT_EXIST_XYZ";
    provider.api_key.clear();
    const auto key = config::ProviderApiKey(provider);
    CHECK_FALSE(key.has_value());
}

// ---------------------------------------------------------------------------
// 鉴权三态(向导重排单):none/env/inline 的解析、迁移、校验、运行时解析。
// ---------------------------------------------------------------------------

TEST_CASE("ParseProviderAuthMode / ProviderAuthModeName: 三态互转,认不得的值报错") {
    CHECK(config::ParseProviderAuthMode("none") == config::ProviderAuthMode::None);
    CHECK(config::ParseProviderAuthMode("env") == config::ProviderAuthMode::Env);
    CHECK(config::ParseProviderAuthMode("inline") == config::ProviderAuthMode::Inline);
    CHECK_FALSE(config::ParseProviderAuthMode("OFF").has_value());
    CHECK_FALSE(config::ParseProviderAuthMode("").has_value());
    CHECK(config::ProviderAuthModeName(config::ProviderAuthMode::None) == "none");
    CHECK(config::ProviderAuthModeName(config::ProviderAuthMode::Env) == "env");
    CHECK(config::ProviderAuthModeName(config::ProviderAuthMode::Inline) == "inline");
}

TEST_CASE("旧配置迁移: 没写 auth 时 api_key 非空算 inline,否则算 env,绝不迁成 none") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[
            {"name":"a","base_url":"https://a.test","wire":"anthropic","api_key":"sk-x"},
            {"name":"b","base_url":"https://b.test","wire":"responses"},
            {"name":"c","base_url":"https://c.test","wire":"responses","key_env":"MISSING_ENV_XYZ"}
        ]})",
        "test.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers->size() == 3);
    CHECK(parsed->providers->at(0).auth == config::ProviderAuthMode::Inline);
    CHECK(parsed->providers->at(1).auth == config::ProviderAuthMode::Env);
    // 环境变量 MISSING_ENV_XYZ 没设置,迁移结果仍是 env——缺 key 不等于无鉴权。
    CHECK(parsed->providers->at(2).auth == config::ProviderAuthMode::Env);
}

TEST_CASE("旧配置迁移: 显式写的 auth 原样生效,落盘值稳定") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[
            {"name":"a","base_url":"https://a.test","wire":"anthropic","auth":"none"},
            {"name":"b","base_url":"https://b.test","wire":"responses","auth":"inline","api_key":"sk-y"}
        ]})",
        "test.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->providers->at(0).auth == config::ProviderAuthMode::None);
    // auth=none 允许 key_env 缺省(默认字段还在,但空串也合法)。
    CHECK(parsed->providers->at(1).auth == config::ProviderAuthMode::Inline);

    // 序列化回写:auth 永远落盘;none 时空 key_env 不落键。
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[]})");
    std::vector<config::ProviderConfig> round_trip = *parsed->providers;
    round_trip[0].key_env.clear();
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), round_trip).has_value());
    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK(written["providers"][0]["auth"] == "none");
    CHECK(written["providers"][1]["auth"] == "inline");
    CHECK_FALSE(written["providers"][0].contains("key_env"));
    CHECK(written["providers"][1].contains("key_env"));

    // 再读回来,auth 三态不丢——重启后仍认得。
    const auto reparsed = config::ParseFileConfigJson(written.dump(), path.string());
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->providers->at(0).auth == config::ProviderAuthMode::None);
    CHECK(reparsed->providers->at(1).auth == config::ProviderAuthMode::Inline);
}

TEST_CASE("ValidateProviderConfig: 按 auth 模式校验,none 允许 key_env 空") {
    config::ProviderConfig provider;
    provider.name = "p";
    provider.base_url = "https://p.test";

    provider.auth = config::ProviderAuthMode::None;
    provider.key_env.clear();
    CHECK(config::ValidateProviderConfig(provider).has_value());

    provider.auth = config::ProviderAuthMode::Env;
    CHECK_FALSE(config::ValidateProviderConfig(provider).has_value());
    provider.key_env = "SOME_ENV";
    CHECK(config::ValidateProviderConfig(provider).has_value());

    provider.auth = config::ProviderAuthMode::Inline;
    CHECK_FALSE(config::ValidateProviderConfig(provider).has_value());
    provider.api_key = "sk-z";
    CHECK(config::ValidateProviderConfig(provider).has_value());
}

TEST_CASE("ResolveProviderAuth: 无需鉴权/已取到/该有却缺三态分清") {
    config::ProviderConfig provider;
    provider.name = "p";
    provider.base_url = "https://p.test";
    provider.key_env = "LUBANCODE_TEST_PROVIDER_KEY_ENV_DOES_NOT_EXIST_XYZ";

    provider.auth = config::ProviderAuthMode::None;
    auto resolved = config::ResolveProviderAuth(provider);
    CHECK(resolved.status == config::ProviderAuthResolution::Status::NotRequired);
    CHECK_FALSE(resolved.key.has_value());

    provider.auth = config::ProviderAuthMode::Env;
    resolved = config::ResolveProviderAuth(provider);
    CHECK(resolved.status == config::ProviderAuthResolution::Status::Missing);
    CHECK(resolved.env_name == "LUBANCODE_TEST_PROVIDER_KEY_ENV_DOES_NOT_EXIST_XYZ");

    provider.auth = config::ProviderAuthMode::Inline;
    provider.api_key = "sk-inline";
    resolved = config::ResolveProviderAuth(provider);
    CHECK(resolved.status == config::ProviderAuthResolution::Status::Ready);
    CHECK(*resolved.key == "sk-inline");

    // 旧优先级兼容:env 模式下 api_key 非空仍优先(一行式 --key 构造的条目)。
    provider.auth = config::ProviderAuthMode::Env;
    resolved = config::ResolveProviderAuth(provider);
    CHECK(resolved.status == config::ProviderAuthResolution::Status::Ready);
    CHECK(*resolved.key == "sk-inline");
}

TEST_CASE("RequireApiKey / RequireConfigured: auth_mode=none 时空 key 放行") {
    config::ConfigResult result;
    result.config.base_url = "https://p.test";
    result.config.model = "m";
    result.config.auth_token = "";

    result.config.auth_mode = config::ProviderAuthMode::Env;
    CHECK_FALSE(config::RequireApiKey(result).has_value());
    CHECK_FALSE(config::RequireConfigured(result).has_value());

    result.config.auth_mode = config::ProviderAuthMode::None;
    CHECK(config::RequireApiKey(result).has_value());
    CHECK(config::RequireConfigured(result).has_value());
}

TEST_CASE("ApplyConfiguredActiveProvider: 镜像 auth_mode,none 时空 auth_token 合法") {
    config::ConfigResult result;
    result.config.active_provider = "bare";
    result.sources.active_provider = config::Source::GlobalConfigFile;
    result.sources.providers = config::Source::GlobalConfigFile;
    config::ProviderConfig provider;
    provider.name = "bare";
    provider.base_url = "https://bare.test";
    provider.wire = config::Wire::ChatCompletions;
    provider.auth = config::ProviderAuthMode::None;
    provider.key_env.clear();
    provider.model = "m";
    result.config.providers = {provider};

    CHECK(config::ApplyConfiguredActiveProvider(result));
    CHECK(result.config.auth_mode == config::ProviderAuthMode::None);
    CHECK(result.config.auth_token.empty());
    CHECK(config::RequireApiKey(result).has_value());
}

TEST_CASE("SetProviderAuthMode: 换模式成功/找不到名字原样不动") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://g.test", .wire = config::Wire::Responses},
    };
    CHECK(config::SetProviderAuthMode(providers, "glm", config::ProviderAuthMode::None));
    CHECK(providers[0].auth == config::ProviderAuthMode::None);
    CHECK(config::SetProviderAuthMode(providers, "glm", config::ProviderAuthMode::Env));
    CHECK(providers[0].auth == config::ProviderAuthMode::Env);
    CHECK_FALSE(config::SetProviderAuthMode(providers, "no-such", config::ProviderAuthMode::None));
    CHECK(providers[0].auth == config::ProviderAuthMode::Env);
}

TEST_CASE("ValidateProviderName: 空名字、非法字符、重名都拦下") {
    const std::vector<config::ProviderConfig> existing{{.name = "dup"}};

    CHECK_FALSE(config::ValidateProviderName("", existing).has_value());
    CHECK_FALSE(config::ValidateProviderName("has space", existing).has_value());
    CHECK_FALSE(config::ValidateProviderName("has/slash", existing).has_value());
    CHECK_FALSE(config::ValidateProviderName("dup", existing).has_value());

    CHECK(config::ValidateProviderName("sub-openai_1.0", existing).has_value());
}

// ---------------------------------------------------------------------------
// extra_body / extra_headers:"任意模型特殊参数"扩展口子。每次请求把
// extra_body 浅合并进请求体顶层(同名覆盖内置字段)、把 extra_headers 加/
// 覆盖到 HTTP 头上——这两个字段本身的解析、合并、落盘,跟 native_web_search
// 系出同门,但 extra_body/extra_headers 是"顶层单 provider 配置"也认的
// (走 hooks/mcp/search/lsp 那套整段替换,不逐键 Source 追踪),这里补全
// 对应测试。
// ---------------------------------------------------------------------------

TEST_CASE("ParseExtraBodyConfig: 合法 object 原样返回") {
    const auto j = nlohmann::json::parse(R"({"thinking":{"type":"enabled"},"reasoning_effort":"max"})");
    const auto result = config::ParseExtraBodyConfig(j, "test.json");
    REQUIRE(result.has_value());
    CHECK(result->at("reasoning_effort") == "max");
    CHECK(result->at("thinking").at("type") == "enabled");
}

TEST_CASE("ParseExtraBodyConfig: 不是 object(数组/字符串/数字)都报错,带上文件路径") {
    for (const nlohmann::json& bad : {nlohmann::json::array({1, 2}), nlohmann::json("x"), nlohmann::json(1)}) {
        const auto result = config::ParseExtraBodyConfig(bad, "my_config.json");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("my_config.json") != std::string::npos);
        CHECK(result.error().find("extra_body") != std::string::npos);
    }
}

TEST_CASE("ParseExtraHeadersConfig: 合法字符串键值对解析正确") {
    const auto j = nlohmann::json::parse(R"({"X-Api-Version":"2024","Authorization":"Bearer xyz"})");
    const auto result = config::ParseExtraHeadersConfig(j, "test.json");
    REQUIRE(result.has_value());
    CHECK(result->at("X-Api-Version") == "2024");
    CHECK(result->at("Authorization") == "Bearer xyz");
}

TEST_CASE("ParseExtraHeadersConfig: 顶层不是 object,或者某个值不是字符串,都报错") {
    const auto not_object = config::ParseExtraHeadersConfig(nlohmann::json::array(), "test.json");
    REQUIRE_FALSE(not_object.has_value());
    CHECK(not_object.error().find("extra_headers") != std::string::npos);

    const auto bad_value = config::ParseExtraHeadersConfig(nlohmann::json::parse(R"({"X-Foo":123})"), "test.json");
    REQUIRE_FALSE(bad_value.has_value());
    CHECK(bad_value.error().find("extra_headers.X-Foo") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: providers 里 extra_body/extra_headers 缺省时是空") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"glm","base_url":"https://a.test","wire":"anthropic"}]})", "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    CHECK(parsed->providers->front().extra_body.empty());
    CHECK(parsed->providers->front().extra_headers.empty());
}

TEST_CASE("ParseFileConfigJson: providers 里 extra_body 空 object、带键两种情况都解得出") {
    const auto empty = config::ParseFileConfigJson(
        R"({"providers":[{"name":"glm","base_url":"https://a.test","wire":"anthropic","extra_body":{}}]})",
        "/tmp/config.json");
    REQUIRE(empty.has_value());
    CHECK(empty->providers->front().extra_body.empty());

    const auto with_keys = config::ParseFileConfigJson(
        R"({"providers":[{"name":"glm","base_url":"https://a.test","wire":"anthropic",)"
        R"("extra_body":{"thinking":{"type":"enabled"},"reasoning_effort":"max"}}]})",
        "/tmp/config.json");
    REQUIRE(with_keys.has_value());
    const auto& body = with_keys->providers->front().extra_body;
    CHECK(body.at("reasoning_effort") == "max");
    CHECK(body.at("thinking").at("type") == "enabled");
}

TEST_CASE("ParseFileConfigJson: providers 里 extra_body/extra_headers 类型不对就报错,不复述两遍路径") {
    const auto bad_body = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","extra_body":[1,2]}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_body.has_value());
    CHECK(bad_body.error().find("extra_body") != std::string::npos);
    // 报错信息里 "配置文件 " 只能出现一次——不能因为 providers[i] 内联校验跟
    // 顶层共用函数没对齐,把路径念重复了。
    CHECK(bad_body.error().find("配置文件") == bad_body.error().rfind("配置文件"));

    const auto bad_headers = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic",)"
        R"("extra_headers":{"X-Foo":123}}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_headers.has_value());
    CHECK(bad_headers.error().find("extra_headers.X-Foo") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson: 顶层(单 provider 扁平配置)也认 extra_body/extra_headers") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"extra_body":{"reasoning_effort":"max"},"extra_headers":{"X-Foo":"bar"}})", "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->extra_body.has_value());
    CHECK(parsed->extra_body->at("reasoning_effort") == "max");
    REQUIRE(parsed->extra_headers.has_value());
    CHECK(parsed->extra_headers->at("X-Foo") == "bar");
}

TEST_CASE("MergeConfig: 顶层 extra_body/extra_headers 没配,最终是空(不启用)") {
    const auto file_config = config::ParseFileConfigJson("{}", "test.json");
    REQUIRE(file_config.has_value());
    const auto merged = config::MergeConfig(config::LubancodeEnvValues{}, *file_config, config::GenericEnvValues{});
    REQUIRE(merged.has_value());
    CHECK(merged->config.extra_body.empty());
    CHECK(merged->config.extra_headers.empty());
}

TEST_CASE("MergeConfig: 顶层 extra_body/extra_headers 配了就整段进最终 Config") {
    const auto file_config = config::ParseFileConfigJson(
        R"({"extra_body":{"reasoning_effort":"max"},"extra_headers":{"X-Foo":"bar"}})", "test.json");
    REQUIRE(file_config.has_value());
    const auto merged = config::MergeConfig(config::LubancodeEnvValues{}, *file_config, config::GenericEnvValues{});
    REQUIRE(merged.has_value());
    CHECK(merged->config.extra_body.at("reasoning_effort") == "max");
    CHECK(merged->config.extra_headers.at("X-Foo") == "bar");
}

TEST_CASE("ProvidersToJson: extra_body/extra_headers 非空才落盘,空的旧配置不长出这两个键") {
    const std::vector<config::ProviderConfig> providers{
        {
            .name = "glm",
            .base_url = "https://open.bigmodel.cn/api/paas/v4",
            .wire = config::Wire::Responses,
            .extra_body = nlohmann::json::parse(R"({"thinking":{"type":"enabled"}})"),
            .extra_headers = {{"X-Foo", "bar"}},
        },
        {
            .name = "minimax",
            .base_url = "https://api.minimax.io/anthropic",
            .wire = config::Wire::Anthropic,
        },
    };
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[]})");
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());

    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK(written["providers"][0]["extra_body"]["thinking"]["type"] == "enabled");
    CHECK(written["providers"][0]["extra_headers"]["X-Foo"] == "bar");
    CHECK_FALSE(written["providers"][1].contains("extra_body"));
    CHECK_FALSE(written["providers"][1].contains("extra_headers"));

    const auto reparsed = config::ParseFileConfigJson(written.dump(), path.string());
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->providers->at(0).extra_body.at("thinking").at("type") == "enabled");
    CHECK(reparsed->providers->at(0).extra_headers.at("X-Foo") == "bar");
    CHECK(reparsed->providers->at(1).extra_body.empty());
    CHECK(reparsed->providers->at(1).extra_headers.empty());
}

TEST_CASE("SetProviderExtraBody: 找得到就整段替换,找不到返回 false 原样不动") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://open.bigmodel.cn/api/paas/v4", .wire = config::Wire::Responses},
    };
    const auto body = nlohmann::json::parse(R"({"reasoning_effort":"max"})");
    CHECK(config::SetProviderExtraBody(providers, "glm", body));
    CHECK(providers[0].extra_body.at("reasoning_effort") == "max");

    // 再设一次,验证是"整段替换"而不是"合并"——旧键应该消失。
    const auto body2 = nlohmann::json::parse(R"({"other_key":"v"})");
    CHECK(config::SetProviderExtraBody(providers, "glm", body2));
    CHECK_FALSE(providers[0].extra_body.contains("reasoning_effort"));
    CHECK(providers[0].extra_body.at("other_key") == "v");

    CHECK_FALSE(config::SetProviderExtraBody(providers, "no-such-provider", body));
    CHECK(providers[0].extra_body.at("other_key") == "v");  // 原样不动
}

TEST_CASE("SetProviderExtraHeader: 设置/覆盖/删除,找不到名字返回 false") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://open.bigmodel.cn/api/paas/v4", .wire = config::Wire::Responses},
    };
    CHECK(config::SetProviderExtraHeader(providers, "glm", "X-Foo", "bar"));
    CHECK(providers[0].extra_headers.at("X-Foo") == "bar");

    // 同名覆盖。
    CHECK(config::SetProviderExtraHeader(providers, "glm", "X-Foo", "baz"));
    CHECK(providers[0].extra_headers.at("X-Foo") == "baz");

    // value 空串 = 删除这一条。
    CHECK(config::SetProviderExtraHeader(providers, "glm", "X-Foo", ""));
    CHECK_FALSE(providers[0].extra_headers.contains("X-Foo"));

    CHECK_FALSE(config::SetProviderExtraHeader(providers, "no-such-provider", "X-Foo", "bar"));
}

TEST_CASE("/provider set extra_body 落盘路径:SetProviderExtraBody 改完再 UpdateProvidersInConfigFile,回读原样") {
    TempCwdDir cwd;
    const std::filesystem::path path = std::filesystem::path(cwd.Path()) / ".lubancode" / "config.json";
    cwd.WriteFile(".lubancode/config.json", R"({"providers":[]})");

    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://open.bigmodel.cn/api/paas/v4", .wire = config::Wire::Responses},
    };
    const auto body = nlohmann::json::parse(R"({"thinking":{"type":"enabled"},"reasoning_effort":"max"})");
    REQUIRE(config::SetProviderExtraBody(providers, "glm", body));
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());

    const nlohmann::json written = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK(written["providers"][0]["extra_body"]["reasoning_effort"] == "max");

    // 清空(设成 {}),落盘应该把这个键去掉,跟 native_web_search=false 不落盘
    // 同一个规矩(空 extra_body 不该占地方)。
    REQUIRE(config::SetProviderExtraBody(providers, "glm", nlohmann::json::object()));
    REQUIRE(config::UpdateProvidersInConfigFile(path.string(), providers).has_value());
    const nlohmann::json cleared = nlohmann::json::parse(cwd.ReadFile(".lubancode/config.json"));
    CHECK_FALSE(cleared["providers"][0].contains("extra_body"));
}

TEST_CASE("ParseFileConfigJson: providers 的 Effort/缓存诊断声明解析与缺省") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"vllm","base_url":"http://127.0.0.1:8000/v1","wire":"chat_completions",)"
        R"("supported_think_levels":["low","medium","xhigh"],"think_param":"reasoning_effort",)"
        R"("think_passthrough":false,"metrics_url":"http://127.0.0.1:8000/metrics"}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->providers.has_value());
    const auto& provider = parsed->providers->front();
    REQUIRE(provider.supported_think_levels.size() == 3);
    CHECK(provider.supported_think_levels[2] == "xhigh");
    CHECK(provider.think_param == "reasoning_effort");
    CHECK(provider.think_passthrough == false);
    CHECK(provider.metrics_url == "http://127.0.0.1:8000/metrics");
    // stream_usage 没写:未声明。
    CHECK(provider.stream_usage_declared == false);

    // 缺省:四个字段都是"没声明",请求构造维持默认。
    const auto bare = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic"}]})", "/tmp/config.json");
    REQUIRE(bare.has_value());
    const auto& plain = bare->providers->front();
    CHECK(plain.supported_think_levels.empty());
    CHECK(plain.think_param.empty());
    CHECK(plain.think_passthrough == true);
    CHECK(plain.metrics_url.empty());
}

TEST_CASE("ParseFileConfigJson: 诊断声明类型不对就报错") {
    const auto bad_levels = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","supported_think_levels":"low"}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_levels.has_value());
    const auto bad_param = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","think_param":123}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_param.has_value());
    const auto bad_passthrough = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"anthropic","think_passthrough":"yes"}]})",
        "/tmp/config.json");
    CHECK_FALSE(bad_passthrough.has_value());
}

TEST_CASE("ParseFileConfigJson: stream_usage 写了键就算声明(显式 false 也是)") {
    const auto parsed = config::ParseFileConfigJson(
        R"({"providers":[{"name":"x","base_url":"https://a.test","wire":"chat_completions","stream_usage":false}]})",
        "/tmp/config.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->providers->front().stream_usage == false);
    CHECK(parsed->providers->front().stream_usage_declared == true);
}

TEST_CASE("ApplyConfiguredActiveProvider: 切 provider 时诊断声明镜像进 Config") {
    config::ConfigResult result;
    result.config.wire = config::Wire::ChatCompletions;
    result.config.base_url = "https://old.test";
    config::ProviderConfig provider;
    provider.name = "vllm";
    provider.base_url = "http://127.0.0.1:8000/v1";
    provider.wire = config::Wire::ChatCompletions;
    provider.supported_think_levels = {"low", "xhigh"};
    provider.think_param = "reasoning.effort";
    provider.think_passthrough = false;
    provider.metrics_url = "http://127.0.0.1:8000/metrics";
    provider.stream_usage = true;
    provider.stream_usage_declared = true;
    result.config.providers = {provider};
    result.config.active_provider = "vllm";

    REQUIRE(config::ApplyConfiguredActiveProvider(result));
    CHECK(result.config.provider_think_levels == std::vector<std::string>{"low", "xhigh"});
    CHECK(result.config.think_param == "reasoning.effort");
    CHECK(result.config.think_passthrough == false);
    CHECK(result.config.metrics_url == "http://127.0.0.1:8000/metrics");
    CHECK(result.config.stream_usage == true);
    CHECK(result.config.stream_usage_declared == true);
}

TEST_CASE("SetProviderStreamUsage: 改值并置声明位,找不到名字返回 false") {
    std::vector<config::ProviderConfig> providers{
        {.name = "glm", .base_url = "https://a.test", .wire = config::Wire::ChatCompletions},
    };
    REQUIRE(config::SetProviderStreamUsage(providers, "glm", true));
    CHECK(providers[0].stream_usage == true);
    CHECK(providers[0].stream_usage_declared == true);
    // 探针写回 false 也算一份显式声明。
    REQUIRE(config::SetProviderStreamUsage(providers, "glm", false));
    CHECK(providers[0].stream_usage == false);
    CHECK(providers[0].stream_usage_declared == true);
    CHECK_FALSE(config::SetProviderStreamUsage(providers, "no-such", true));
}

// ---------------------------------------------------------------------------
// goals 段与 features.goals(持久目标单):duration 折毫秒、坏值按默认收、
// 段类型不对报错。
// ---------------------------------------------------------------------------

TEST_CASE("goals 配置:默认关、默认预算;features.goals 打开") {
    const auto result = config::MergeConfig(EmptyLubancodeEnv(), std::nullopt, EmptyGenericEnv());
    REQUIRE(result.has_value());
    CHECK(result->config.features_goals == false);
    CHECK(result->config.goals.max_elapsed_ms == 2 * 60 * 60 * 1000);
    CHECK(result->config.goals.max_iterations == 40);
    CHECK(result->config.goals.max_no_progress_iterations == 3);
    CHECK(result->config.goals.max_same_blocker_iterations == 3);
    CHECK(result->config.goals.max_consecutive_provider_failures == 3);
    CHECK(result->sources.goals == config::Source::Default);

    const auto parsed = config::ParseFileConfigJson(
        R"({"features":{"goals":true},"goals":{"max_elapsed":"90m","max_iterations":12,"max_no_progress_iterations":2}})",
        "test.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->features_goals.has_value());
    CHECK(*parsed->features_goals == true);
    const auto merged = config::MergeConfig(EmptyLubancodeEnv(), *parsed, EmptyGenericEnv());
    REQUIRE(merged.has_value());
    CHECK(merged->config.features_goals == true);
    CHECK(merged->config.goals.max_elapsed_ms == 90 * 60 * 1000);
    CHECK(merged->config.goals.max_iterations == 12);
    CHECK(merged->config.goals.max_no_progress_iterations == 2);
    CHECK(merged->config.goals.max_same_blocker_iterations == 3);
    CHECK(merged->sources.goals == config::Source::ProjectConfigFile);
}

TEST_CASE("goals 配置:duration 各单位与坏值") {
    struct Case {
        const char* text;
        std::int64_t expect_ms;
    };
    const Case cases[] = {
        {"2h", 7200000}, {"90m", 5400000}, {"45s", 45000}, {"30", 30000},
        {"1d", 86400000}, {"  10m  ", 600000},
    };
    for (const auto& c : cases) {
        const auto parsed = config::ParseFileConfigJson(
            std::string(R"({"goals":{"max_elapsed":")") + c.text + R"("}})", "t.json");
        REQUIRE(parsed.has_value());
        const auto merged = config::MergeConfig(EmptyLubancodeEnv(), *parsed, EmptyGenericEnv());
        REQUIRE(merged.has_value());
        CHECK(merged->config.goals.max_elapsed_ms == c.expect_ms);
    }
    for (const char* bad : {"abc", "m", ""}) {
        const auto parsed = config::ParseFileConfigJson(
            std::string(R"({"goals":{"max_elapsed":")") + bad + R"("}})", "t.json");
        REQUIRE(parsed.has_value());
        const auto merged = config::MergeConfig(EmptyLubancodeEnv(), *parsed, EmptyGenericEnv());
        REQUIRE(merged.has_value());
        CHECK(merged->config.goals.max_elapsed_ms == 2 * 60 * 60 * 1000);
    }
}

TEST_CASE("goals 配置:段类型不对报错") {
    const auto bad = config::ParseFileConfigJson(R"({"features":"on"})", "t.json");
    CHECK_FALSE(bad.has_value());
    const auto bad2 = config::ParseFileConfigJson(R"({"goals":[1]})", "t.json");
    CHECK_FALSE(bad2.has_value());
    const auto bad3 = config::ParseFileConfigJson(R"({"features":{"goals":"yes"}})", "t.json");
    CHECK_FALSE(bad3.has_value());
    const auto bad4 = config::ParseFileConfigJson(R"({"goals":{"max_iterations":-1}})", "t.json");
    CHECK_FALSE(bad4.has_value());
}
