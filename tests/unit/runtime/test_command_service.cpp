// CommandService 单测(显示系统剥离单第七步:拆命令服务)。
//
// 钉的是三条 typed API(/model、/resume、审批回答)不经 slash 字符串的
// 完整调用(单子验收原文):
//   1. QueryModels/SetModel:清单、当前项、目录条目应用、显式写回;
//   2. ListThreads/ResumeThread:序号与 id 解析、回放接管、旧账(标题/
//      压缩序号/落盘基线)接上;
//   3. ResolveApproval/AnswerQuestion:四态转发;迟到回答 stale;没有
//      broker 的前端(终端当场问完)明说,不装成功。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "sessions/session_store.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "runtime/command_service.hpp"
#include "runtime/interaction_broker.hpp"
#include "runtime/session_runtime.hpp"
#include "tools/registry.hpp"

namespace rt = lubancode::runtime;
using namespace lubancode;

namespace {

class TempSessionsDir {
public:
    TempSessionsDir() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("lubancode-command-service-" + std::to_string(counter_++)))
                    .string();
        std::filesystem::create_directories(path_);
    }
    ~TempSessionsDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }

private:
    static inline int counter_ = 0;
    std::string path_;
};

// 一场真存档:Begin + 两条消息,给 resume 接管。
std::string WriteSampleSession(const std::string& dir, const std::string& slug) {
    sessions::SessionStore store(dir);
    sessions::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "old-model";
    meta.cwd = "/tmp";
    meta.started_at = sessions::NowTimestamp();
    REQUIRE(store.Begin(meta, slug));
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"旧话"});
    store.AppendMessage(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"旧答"});
    store.AppendMessage(assistant);
    return store.session_id();
}

class NullBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>&,
                                                const std::atomic<bool>* = nullptr) override {
        return std::unexpected(api::Error{api::ErrorKind::Api, "null", 0});
    }
};

rt::CommandService::Options BaseOptions() {
    rt::CommandService::Options options;
    options.current_model = std::make_shared<std::string>("m1");
    options.current_think = std::make_shared<std::string>("low");
    options.fetch_models = []() -> std::expected<std::vector<std::pair<std::string, std::string>>, std::string> {
        return std::vector<std::pair<std::string, std::string>>{{"m1", "一号"}, {"m2", "二号"}};
    };
    return options;
}

// 最小 Broker 记录实现:ResolveApproval 只认一次(答完即失效,迟到 stale),
// AnswerQuestion 恒不认(演示 stale 路)。
class RecordingBroker final : public rt::InteractionBroker {
public:
    std::shared_ptr<rt::InteractionFuture> AskApproval(const rt::ApprovalRequest&) override {
        return nullptr;
    }
    std::shared_ptr<rt::InteractionFuture> AskQuestion(const rt::QuestionRequest&) override {
        return nullptr;
    }
    bool ResolveApproval(const rt::InteractionRequestId& id, const rt::ApprovalResponse&) override {
        if (id.value == pending_id_) {
            pending_id_.clear();  // 答完即失效:再答就是迟到
            return true;
        }
        return false;
    }
    bool AnswerQuestion(const rt::InteractionRequestId&, const rt::QuestionResponse&) override {
        return false;  // 没有挂起的提问
    }
    std::string pending_id_ = "req-1";
};

}  // namespace

TEST_CASE("SetModel:query 清单带当前项,提交切模型并应用目录条目") {
    config::Config config;
    config.model = "m1";
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;

    rt::CommandService service(options);
    const auto query = service.QueryModels();
    REQUIRE(query.models.size() == 2);
    CHECK(query.models[0].id == "m1");
    CHECK(query.models[0].current);
    CHECK_FALSE(query.models[1].current);
    CHECK(query.current_model == "m1");
    CHECK_FALSE(query.fetch_failed);

    const auto result = service.SetModel("m2", /*write_config=*/false);
    CHECK(result.switched);
    CHECK(result.model == "m2");
    CHECK(config.model == "m2");
    CHECK(*options.current_model == "m2");
    CHECK(result.error.empty());
    // 再 query:当前项换人。
    CHECK(service.QueryModels().models[1].current);
}

TEST_CASE("SetModel:空 id 不切;取数失败如实报") {
    rt::CommandService::Options options = BaseOptions();
    options.fetch_models = []() -> std::expected<std::vector<std::pair<std::string, std::string>>, std::string> {
        return std::unexpected("网络炸了");
    };
    rt::CommandService service(options);
    const auto query = service.QueryModels();
    CHECK(query.fetch_failed);
    CHECK(query.fetch_error == "网络炸了");
    CHECK(query.models.empty());

    const auto result = service.SetModel("", false);
    CHECK_FALSE(result.switched);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("SetModel:目录条目应用 default_think") {
    config::ModelCatalog catalog;
    config::ModelCatalogEntry entry;
    entry.slug = "m2";
    entry.default_think = "high";
    catalog.models.push_back(entry);

    config::Config config;
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;
    options.model_catalog = &catalog;
    rt::CommandService service(options);
    const auto result = service.SetModel("m2", false);
    CHECK(result.switched);
    CHECK(result.think == "high");
    CHECK(result.think_from_catalog);
    CHECK(*options.current_think == "high");
    // 不在目录的模型:think 不动,回执不报。
    const auto stay = service.SetModel("m1", false);
    CHECK(stay.switched);
    CHECK(*options.current_think == "high");
    CHECK_FALSE(stay.think_from_catalog);
}

TEST_CASE("SetModel:目录窗口经回调落账,base_instructions 整体替换") {
    config::ModelCatalog catalog;
    config::ModelCatalogEntry entry;
    entry.slug = "m2";
    entry.default_think = "max";
    entry.context_window_tokens = 1048576;
    entry.base_instructions = "m2 专属指令";
    catalog.models.push_back(entry);

    config::Config config;
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;
    options.model_catalog = &catalog;
    options.current_model_instructions = std::make_shared<std::string>("旧模型指令");
    std::size_t applied_window = 0;
    options.apply_context_window = [&](std::size_t tokens) { applied_window = tokens; };
    rt::CommandService service(options);

    const auto result = service.SetModel("m2", false);
    REQUIRE(result.switched);
    CHECK(result.applied_context_window == std::optional<std::size_t>(1048576U));
    CHECK(applied_window == 1048576U);  // 回调真被调过,不只是回执报数
    CHECK(result.instructions_replaced);
    CHECK(*options.current_model_instructions == "m2 专属指令");

    // 切到目录外:窗口不再动(回调不响),旧指令被清空但不算"替换成新指令"。
    const auto outside = service.SetModel("m9", false);
    REQUIRE(outside.switched);
    CHECK_FALSE(outside.think_from_catalog);
    CHECK_FALSE(outside.applied_context_window.has_value());
    CHECK(applied_window == 1048576U);
    CHECK_FALSE(outside.instructions_replaced);
    CHECK(options.current_model_instructions->empty());
}

TEST_CASE("WriteModelToConfig:先切后写两步走,provider 条目吃下模型") {
    TempSessionsDir dir;
    const std::string cfg_path = dir.path() + "/config.json";
    {
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out << "{\"model\":\"top-m\",\"active_provider\":\"ds\",\"providers\":["
               "{\"name\":\"ds\",\"wire\":\"anthropic-messages\",\"base_url\":\"http://x\",\"model\":\"ds-old\"}]}";
    }

    config::Config config;
    config.model = "top-m";
    config.active_provider = "ds";
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;
    options.config_file_path = cfg_path;
    rt::CommandService service(options);

    // 第一步:只切不写。
    const auto result = service.SetModel("ds-flash", /*write_config=*/false);
    REQUIRE(result.switched);
    CHECK_FALSE(result.config_written);
    // 第二步:问完 y 再单独落盘——与 SetModel(write=true) 同一段写盘逻辑。
    const auto written = service.WriteModelToConfig("ds-flash");
    REQUIRE(written.has_value());
    {
        std::ifstream in(cfg_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buffer.str());
        CHECK(parsed["providers"][0]["model"] == "ds-flash");
        CHECK(parsed["model"] == "top-m");  // 顶层不动:活跃端镜像压过它
    }

    // 没配 config_file_path 的 service:如实报错,不装成功。
    rt::CommandService::Options bare_options = BaseOptions();
    bare_options.config = &config;
    rt::CommandService bare_service(bare_options);
    CHECK_FALSE(bare_service.WriteModelToConfig("m1").has_value());
}

TEST_CASE("SetRoleModel:改内存 shorthand,角色名归一,不认的名字如实报") {
    config::Config config;
    config.model = "m1";
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;

    rt::CommandService service(options);

    // cheap 设置 + 大写归一。
    const auto cheap = service.SetRoleModel("CHEAP", "mini-1", /*write_config=*/false);
    REQUIRE(cheap.switched);
    CHECK(cheap.role == "cheap");
    CHECK(cheap.model == "mini-1");
    CHECK(config.cheap_model == "mini-1");

    // plan 是 lao 的别名。
    const auto lao = service.SetRoleModel("plan", "big-1", false);
    REQUIRE(lao.switched);
    CHECK(lao.role == "lao");
    CHECK(config.lao_model == "big-1");

    // normal 同路。
    const auto normal = service.SetRoleModel("Normal", "m2", false);
    REQUIRE(normal.switched);
    CHECK(config.normal_model == "m2");

    // 不认的角色名/空模型名:不切、带稳定码。
    const auto bad = service.SetRoleModel("turbo", "x", false);
    CHECK_FALSE(bad.switched);
    CHECK(bad.error == "unknown_role");
    const auto empty = service.SetRoleModel("cheap", "", false);
    CHECK_FALSE(empty.switched);
    CHECK(empty.error == "empty_model");
}

TEST_CASE("SetRoleModel:落盘 shorthand;高级段在场且该格已配时改高级段") {
    TempSessionsDir dir;  // 借它的临时目录,存配置文件
    const std::string cfg_path = dir.path() + "/config.json";

    {  // 先造一份带 model_roles 高级段(仅 normal 格)的文件
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out << "{\"model\":\"m1\","
               "\"model_roles\":{\"normal\":{\"model\":\"n-1\",\"effort\":\"low\"}}}";
    }

    config::Config config;
    config.model = "m1";
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;
    options.config_file_path = cfg_path;
    rt::CommandService service(options);

    // cheap 没有高级段格:落 shorthand 字段。
    const auto cheap = service.SetRoleModel("cheap", "mini-9", /*write_config=*/true);
    REQUIRE(cheap.switched);
    CHECK(cheap.config_written);
    {
        std::ifstream in(cfg_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buffer.str());
        CHECK(parsed["cheap_model"] == "mini-9");
        CHECK(parsed["model"] == "m1");  // 别的字段原样保留
        // 高级段只有 normal 格,没被动过。
        CHECK(parsed["model_roles"]["normal"]["model"] == "n-1");
        CHECK(parsed["model_roles"]["normal"]["effort"] == "low");
    }

    // normal 高级段已配:改高级段那格,不写 shorthand。
    const auto normal = service.SetRoleModel("normal", "n-2", true);
    REQUIRE(normal.switched);
    CHECK(normal.config_written);
    {
        std::ifstream in(cfg_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buffer.str());
        CHECK(parsed["model_roles"]["normal"]["model"] == "n-2");
        CHECK(parsed["model_roles"]["normal"]["effort"] == "low");  // 其余字段保留
        CHECK_FALSE(parsed.contains("normal_model"));
    }
}

TEST_CASE("SetModel:active_provider 在场写 provider 条目,各记各的模型") {
    TempSessionsDir dir;
    const std::string cfg_path = dir.path() + "/config.json";
    {
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out << "{\"model\":\"top-m\",\"active_provider\":\"ds\",\"providers\":["
               "{\"name\":\"ds\",\"wire\":\"anthropic-messages\",\"base_url\":\"http://x\",\"model\":\"ds-old\"},"
               "{\"name\":\"glm\",\"wire\":\"anthropic-messages\",\"base_url\":\"http://y\",\"model\":\"glm-old\"}]}";
    }

    config::Config config;
    config.model = "top-m";
    config.active_provider = "ds";
    rt::CommandService::Options options = BaseOptions();
    options.config = &config;
    options.config_file_path = cfg_path;
    rt::CommandService service(options);

    // 活跃端在场:写 ds 条目的 model,顶层 model 与别的条目一概不动。
    const auto entry = service.SetModel("ds-flash", /*write_config=*/true);
    REQUIRE(entry.switched);
    CHECK(entry.config_written);
    {
        std::ifstream in(cfg_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buffer.str());
        CHECK(parsed["providers"][0]["model"] == "ds-flash");
        CHECK(parsed["providers"][0]["base_url"] == "http://x");  // 其余键保留
        CHECK(parsed["providers"][1]["model"] == "glm-old");
        CHECK(parsed["model"] == "top-m");
        CHECK(parsed["active_provider"] == "ds");
    }

    // active_provider 空:退回写顶层 model。
    config.active_provider = "";
    const auto top = service.SetModel("top-new", true);
    REQUIRE(top.switched);
    CHECK(top.config_written);
    {
        std::ifstream in(cfg_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buffer.str());
        CHECK(parsed["model"] == "top-new");
        CHECK(parsed["providers"][0]["model"] == "ds-flash");  // 条目不再被动
    }
}

TEST_CASE("ResumeThread:序号、id、空串三条解析路,旧账接上") {
    TempSessionsDir dir;
    const std::string id_a = WriteSampleSession(dir.path(), "第一场");
    const std::string id_b = WriteSampleSession(dir.path(), "第二场");

    NullBackend backend;
    tools::ToolRegistry registry;
    agent::AgentRuntimeProfile profile;
    agent::Agent loop(backend, registry, std::move(profile), std::string("resume-test"));
    rt::SessionRuntime runtime({dir.path(), "anthropic", "20260823-130000"});

    rt::CommandService::Options options = BaseOptions();
    options.sessions_dir = dir.path();
    rt::CommandService service(options);

    // 列表:倒序,第二场在前。
    const auto list = service.ListThreads();
    REQUIRE(list.size() == 2);
    CHECK(list[0].index == 1);
    CHECK(list[0].id == id_b);
    CHECK(list[1].id == id_a);

    // 按序号恢复(1 = 最近一场)。
    auto result = service.ResumeThread(loop, runtime, "1", "/tmp");
    CHECK(result.resumed);
    CHECK(result.id == id_b);
    CHECK(result.restored_messages == 2);
    CHECK(loop.History().size() == 2);
    CHECK(runtime.store().active());
    CHECK(runtime.persisted_count() == 2);
    CHECK(runtime.meta().model == "old-model");

    // 按 id 恢复(旧场)。
    result = service.ResumeThread(loop, runtime, id_a, "/tmp");
    CHECK(result.resumed);
    CHECK(result.id == id_a);
    CHECK(runtime.title().empty());  // 没写过 title 事件

    // 空串 = 最近一场。
    result = service.ResumeThread(loop, runtime, "", "/tmp");
    CHECK(result.resumed);
    CHECK(result.id == id_b);

    // 越界序号:如实拒。
    CHECK_FALSE(service.ResumeThread(loop, runtime, "9", "/tmp").resumed);
}

TEST_CASE("ResolveApproval/AnswerQuestion:四态转发,迟到 stale,无 broker 明说") {
    rt::CommandService::Options options = BaseOptions();
    const rt::CommandService service(options);
    RecordingBroker broker;

    rt::ApprovalResponse accept;
    accept.decision = rt::InteractionDecision::Accept;
    auto answer = service.ResolveApproval(&broker, "req-1", accept);
    CHECK(answer.ok);
    CHECK(answer.error_code.empty());

    // 迟到:同 id 再答,stale。
    answer = service.ResolveApproval(&broker, "req-1", accept);
    CHECK_FALSE(answer.ok);
    CHECK(answer.error_code == rt::kStaleRequestId);

    // 提问同款。
    rt::QuestionResponse question;
    question.answers = {"选项一"};
    auto q_answer = service.AnswerQuestion(&broker, "req-2", question);
    CHECK_FALSE(q_answer.ok);
    CHECK(q_answer.error_code == rt::kStaleRequestId);

    // 没有 broker 的前端(终端当场问完):不装成功。
    answer = service.ResolveApproval(nullptr, "req-1", accept);
    CHECK_FALSE(answer.ok);
    CHECK(answer.error_code == "no_broker");
}
