// hook 包校验与试跑(LuaHook 单 P1-D,§8.2)的实现。分档合同见头文件。
// 铁律:不为校验默认访问外部服务或写业务文件——HTTP/工具全走 fixture
// 编排的 fake adapter;文件写只落临时目录。
#include "runtime/hook_package_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <set>
#include <system_error>

#include "hooks/middleware.hpp"
#include "hooks/middleware_builtins.hpp"
#include "hooks/middleware_loader.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/plugin_http.hpp"
#include "runtime/plugin_lua_host.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::runtime {

namespace {

using hooks::middleware::DispatchOutcome;
using hooks::middleware::DispatchTrigger;
using hooks::middleware::HookPoint;
using hooks::middleware::MiddlewareDefinition;
using hooks::middleware::MiddlewareDispatcher;
using hooks::middleware::MiddlewarePool;
using hooks::middleware::ParseHookPoint;

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::nullopt;
    }
    return content;
}

// fixture 的 fake 工具:按编排回一段正文(§8.2 "宿主工具默认 fake adapter")。
// "outcome" 给了(如 timed_out)按终态分型路回错——unknown/cancelled 分支
// 的样例靠它编排,不动网。
class FixtureFakeTool final : public tools::Tool {
public:
    FixtureFakeTool(std::string name, std::string description, nlohmann::json schema, std::string content,
                    bool is_error, std::string outcome)
        : name_(std::move(name)), description_(std::move(description)), schema_(std::move(schema)),
          content_(std::move(content)), is_error_(is_error || !outcome.empty()), outcome_(std::move(outcome)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return description_; }
    nlohmann::json input_schema() const override { return schema_; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        if (!outcome_.empty()) {
            tools::Tool::Result result = tools::Tool::Result::Error(content_);
            result.outcome = outcome_;
            return result;
        }
        return tools::Tool::Result(content_, is_error_);
    }

private:
    std::string name_;
    std::string description_;
    nlohmann::json schema_;
    std::string content_;
    bool is_error_;
    std::string outcome_;
};

// JSON 子集比较:expected 里每个键都在 actual 里等值。数组与对象递归同口径;
// expected 为非 object(标量/数组)时走整体相等。
bool JsonSubsetMatches(const nlohmann::json& expected, const nlohmann::json& actual) {
    if (expected.is_object() && actual.is_object()) {
        for (const auto& item : expected.items()) {
            const auto it = actual.find(item.key());
            if (it == actual.end() || !JsonSubsetMatches(item.value(), *it)) {
                return false;
            }
        }
        return true;
    }
    return expected == actual;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) {
            out += "; ";
        }
        out += line;
    }
    return out;
}

// 断言 expect 对 outcome;全过回空,挂了回原因清单。
std::vector<std::string> CheckExpectation(const nlohmann::json& expect, const DispatchOutcome& outcome) {
    std::vector<std::string> failures;
    if (const auto it = expect.find("kind"); it != expect.end() && it->is_string()) {
        const std::string expected_kind = it->get<std::string>();
        const char* actual = outcome.kind == DispatchOutcome::Kind::Completed  ? "completed"
                             : outcome.kind == DispatchOutcome::Kind::Denied ? "denied"
                                                                             : "failed";
        if (expected_kind != actual) {
            failures.push_back("kind 期望 " + expected_kind + ",实际 " + actual);
        }
    }
    if (const auto it = expect.find("prompt"); it != expect.end()) {
        const auto prompt = outcome.adopted_input.find("prompt");
        if (prompt == outcome.adopted_input.end() || !JsonSubsetMatches(*it, *prompt)) {
            failures.push_back("adopted prompt 期望 " + it->dump() + ",实际 " +
                               (prompt == outcome.adopted_input.end() ? "(缺)"
                                                                      : prompt->dump()));
        }
    }
    if (const auto it = expect.find("adoptedInput"); it != expect.end()) {
        if (!JsonSubsetMatches(*it, outcome.adopted_input)) {
            failures.push_back("adoptedInput 期望 " + it->dump() + ",实际 " + outcome.adopted_input.dump());
        }
    }
    if (const auto it = expect.find("value"); it != expect.end()) {
        if (!JsonSubsetMatches(*it, outcome.value)) {
            failures.push_back("value 期望 " + it->dump() + ",实际 " + outcome.value.dump());
        }
    }
    if (const auto it = expect.find("denyCode"); it != expect.end() && it->is_string()) {
        if (outcome.deny_code != it->get<std::string>()) {
            failures.push_back("denyCode 期望 " + it->get<std::string>() + ",实际 " + outcome.deny_code);
        }
    }
    if (const auto it = expect.find("errorCode"); it != expect.end() && it->is_string()) {
        if (outcome.error_code != it->get<std::string>()) {
            failures.push_back("errorCode 期望 " + it->get<std::string>() + ",实际 " + outcome.error_code);
        }
    }
    if (const auto it = expect.find("contextAppends"); it != expect.end() && it->is_array()) {
        if (outcome.context_appends.size() != it->size()) {
            failures.push_back("contextAppends 期望 " + std::to_string(it->size()) + " 条,实际 " +
                               std::to_string(outcome.context_appends.size()) + " 条");
        } else {
            for (std::size_t i = 0; i < it->size(); ++i) {
                if (!(*it)[i].is_string() || outcome.context_appends[i] != (*it)[i].get<std::string>()) {
                    failures.push_back("contextAppends[" + std::to_string(i) + "] 不符: 期望 " +
                                       (*it)[i].dump() + ",实际 " + outcome.context_appends[i]);
                }
            }
        }
    }
    // 子串档:动态内容(executionId 一类)用包含断言;每段子串至少命中
    // 一条已采用的 append。
    if (const auto it = expect.find("contextAppendsContain"); it != expect.end() && it->is_array()) {
        for (const auto& needle : *it) {
            if (!needle.is_string()) {
                continue;
            }
            bool found = false;
            for (const std::string& append : outcome.context_appends) {
                if (append.find(needle.get<std::string>()) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                failures.push_back("contextAppendsContain 没命中: " + needle.get<std::string>());
            }
        }
    }
    if (const auto it = expect.find("records"); it != expect.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            const hooks::middleware::InvocationRecord* record = outcome.FindRecord(item.key());
            if (record == nullptr) {
                failures.push_back("records 里没有 " + item.key());
                continue;
            }
            if (const auto outcome_it = item.value().find("outcome");
                outcome_it != item.value().end() && outcome_it->is_string()) {
                if (record->outcome != outcome_it->get<std::string>()) {
                    failures.push_back(item.key() + " outcome 期望 " + outcome_it->get<std::string>() +
                                       ",实际 " + record->outcome);
                }
            }
            if (const auto calls_it = item.value().find("nextCalls");
                calls_it != item.value().end() && calls_it->is_number_integer()) {
                if (record->next_calls != calls_it->get<int>()) {
                    failures.push_back(item.key() + " nextCalls 期望 " +
                                       std::to_string(calls_it->get<int>()) + ",实际 " +
                                       std::to_string(record->next_calls));
                }
            }
            if (const auto consumed_it = item.value().find("nextConsumed");
                consumed_it != item.value().end() && consumed_it->is_boolean()) {
                if (record->next_consumed != consumed_it->get<bool>()) {
                    failures.push_back(item.key() + " nextConsumed 期望 " +
                                       (consumed_it->get<bool>() ? "true" : "false") + ",实际 " +
                                       (record->next_consumed ? "true" : "false"));
                }
            }
            if (const auto effects_it = item.value().find("effects");
                effects_it != item.value().end() && effects_it->is_array()) {
                if (record->effects.size() != effects_it->size()) {
                    failures.push_back(item.key() + " effects 期望 " + std::to_string(effects_it->size()) +
                                       " 枚,实际 " + std::to_string(record->effects.size()) + " 枚");
                } else {
                    for (std::size_t i = 0; i < effects_it->size(); ++i) {
                        const nlohmann::json& expected = (*effects_it)[i];
                        if (expected.contains("type") &&
                            record->effects[i].type != expected.at("type").get<std::string>()) {
                            failures.push_back(item.key() + " effects[" + std::to_string(i) + "] type 期望 " +
                                               expected.at("type").dump() + ",实际 " + record->effects[i].type);
                        }
                        if (expected.contains("applied") && record->effects[i].applied != expected.at("applied")) {
                            failures.push_back(item.key() + " effects[" + std::to_string(i) + "] applied 期望 " +
                                               expected.at("applied").dump() + ",实际 " +
                                               (record->effects[i].applied ? "true" : "false"));
                        }
                        if (expected.contains("payload") && !expected.at("payload").is_null() &&
                            !JsonSubsetMatches(expected.at("payload"), record->effects[i].payload)) {
                            failures.push_back(item.key() + " effects[" + std::to_string(i) + "] payload 期望 " +
                                               expected.at("payload").dump() + ",实际 " +
                                               record->effects[i].payload.dump());
                        }
                    }
                }
            }
        }
    }
    if (const auto it = expect.find("terminalRuns"); it != expect.end() && it->is_number_integer()) {
        if (outcome.terminal_runs != it->get<int>()) {
            failures.push_back("terminalRuns 期望 " + std::to_string(it->get<int>()) + ",实际 " +
                               std::to_string(outcome.terminal_runs));
        }
    }
    return failures;
}

std::filesystem::path MakeScratchDir(const std::optional<std::filesystem::path>& root) {
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long seq = counter.fetch_add(1) + 1;
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path dir = root.value_or(std::filesystem::temp_directory_path()) /
                                      ("lubancode-hook-check-" + std::to_string(stamp) + "-" +
                                       std::to_string(seq));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace

// ---------------------------------------------------------------------------
// fixture 试跑(fake adapter 档)
// ---------------------------------------------------------------------------

namespace {

// 一枚 fixture 的完整试跑环境:fresh pool/center/transport/registry(用例
// 间不串状态,§8.2 "断言候选、效果、next 次数、顺序和错误")。
struct FixtureRig {
    FakeHttpTransport transport;
    tools::ToolRegistry registry;
    std::shared_ptr<HookStateStore> state = std::make_shared<HookStateStore>();
    std::vector<std::string> logs;
    HookHostServiceCenter center;
    std::filesystem::path scratch;

    explicit FixtureRig(const std::filesystem::path& package_dir,
                        const std::optional<std::filesystem::path>& scratch_root,
                        const nlohmann::json& fixture) {
        // fake HTTP:fixture 编排 canned 响应;耗尽后 NetworkFailed(不动网)。
        if (const auto it = fixture.find("http"); it != fixture.end() && it->is_array()) {
            for (const auto& scripted : *it) {
                HttpExchangeResponse response;
                response.status = scripted.value("status", 200);
                if (const auto body = scripted.find("body"); body != scripted.end() && body->is_string()) {
                    response.body = body->get<std::string>();
                } else if (const auto json = scripted.find("json"); json != scripted.end()) {
                    response.body = json->dump();
                }
                response.final_url = scripted.value("url", std::string("https://fake.local/hook-fixture"));
                transport.EnqueueResponse(std::move(response));
            }
        }
        // fake 工具:fixture 点名谁注册谁;allow_tools 同名单(发现=授权,
        // 校验场里不设第二套门槛)。
        std::vector<std::string> allow_tools;
        if (const auto it = fixture.find("tools"); it != fixture.end() && it->is_array()) {
            for (const auto& tool_spec : *it) {
                const std::string tool_name = tool_spec.value("name", std::string());
                if (tool_name.empty()) {
                    continue;
                }
                allow_tools.push_back(tool_name);
                registry.Register(std::make_unique<FixtureFakeTool>(
                    tool_name, tool_spec.value("description", std::string("fixture fake tool")),
                    tool_spec.value("schema", nlohmann::json::object()),
                    tool_spec.value("content", std::string("")), tool_spec.value("isError", false),
                    tool_spec.value("outcome", std::string())));
            }
        }
        scratch = MakeScratchDir(scratch_root);

        HookHostServiceCenter::Grants grants;
        PluginHttpCallSpec http;
        http.transport = &transport;
        http.limits = EffectiveHttpLimits{};
        grants.http = std::move(http);
        grants.fs = HookFsGrant{package_dir, /*read_roots=*/{package_dir}, /*write_roots=*/{scratch}};
        grants.state = state;
        grants.log = [this](const std::string& level, const nlohmann::json& fields) {
            logs.push_back(level + " " + fields.dump());
        };
        grants.tool_registry = &registry;
        grants.allow_tools = std::move(allow_tools);
        center.SetGrants(std::move(grants));
    }

    ~FixtureRig() {
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }
};

HookCheckReport::Fixture RunSingleFixture(const nlohmann::json& fixture, const std::string& fallback_name,
                                          const std::filesystem::path& package_dir,
                                          const std::optional<std::filesystem::path>& scratch_root,
                                          const std::vector<MiddlewareDefinition>& defs,
                                          const nlohmann::json& manifest) {
    HookCheckReport::Fixture result;
    result.name = fixture.value("name", fallback_name);

    // 挂点:fixture 点名 > 清单第一条。
    HookPoint point = HookPoint::PreUser;
    if (const auto it = fixture.find("hookPoint"); it != fixture.end() && it->is_string()) {
        if (!ParseHookPoint(it->get<std::string>(), point)) {
            result.detail = "fixture hookPoint 不认识: " + it->get<std::string>();
            return result;
        }
    } else {
        const auto hooks_it = manifest.find("hooks");
        const std::string manifest_point =
            hooks_it != manifest.end() && hooks_it->is_array() && !hooks_it->empty()
                ? (*hooks_it)[0].value("hookPoint", std::string())
                : std::string();
        if (manifest_point.empty() || !ParseHookPoint(manifest_point, point)) {
            result.detail = "挂点缺失(fixture 未给,清单 hooks 里也取不出)";
            return result;
        }
    }

    // trigger 必有 input。
    const auto trigger_it = fixture.find("trigger");
    if (trigger_it == fixture.end() || !trigger_it->is_object() || !trigger_it->contains("input")) {
        result.detail = "fixture 缺 trigger.input";
        return result;
    }

    // 组链:内置槽 + 本包定义;失败即 fixture 挂(静态已过,这里挂多为
    // fixture 自身的组装问题)。
    FixtureRig rig(package_dir, scratch_root, fixture);
    MiddlewarePool::Options pool_options;
    HookHostServiceCenter* center_ptr = &rig.center;
    pool_options.lua_factory = [center_ptr](const hooks::middleware::LuaHandlerSpec& spec,
                                            const hooks::middleware::HandlerLimits& limits) {
        return MakeLuaHookHandler(spec, limits, center_ptr);
    };
    MiddlewarePool pool(std::move(pool_options));
    hooks::middleware::AddBuiltinRequestSlots(pool);
    for (const MiddlewareDefinition& def : defs) {
        pool.AddDefinition(def);
    }
    auto published = pool.Publish();
    if (!published.has_value()) {
        result.detail = "注册表发布失败: " + published.error().code + " " + published.error().message;
        return result;
    }
    MiddlewareDispatcher dispatcher(std::move(*published));

    DispatchTrigger trigger;
    trigger.input = trigger_it->at("input");
    if (const auto it = trigger_it->find("origin"); it != trigger_it->end() && it->is_string()) {
        trigger.origin = it->get<std::string>();
    }
    if (const auto it = trigger_it->find("purpose"); it != trigger_it->end() && it->is_string()) {
        trigger.purpose = it->get<std::string>();
    }
    if (const auto it = trigger_it->find("deliveryMode"); it != trigger_it->end() && it->is_string()) {
        trigger.delivery_mode = it->get<std::string>();
    }
    if (const auto it = trigger_it->find("turnId"); it != trigger_it->end() && it->is_string()) {
        trigger.turn_id = it->get<std::string>();
    }
    if (const auto it = trigger_it->find("stepId"); it != trigger_it->end() && it->is_string()) {
        trigger.step_id = it->get<std::string>();
    }
    if (const auto it = trigger_it->find("actionId"); it != trigger_it->end() && it->is_string()) {
        trigger.action_id = it->get<std::string>();
    }
    if (const auto it = trigger_it->find("requestId"); it != trigger_it->end() && it->is_string()) {
        trigger.request_id = it->get<std::string>();
    }
    // PreRequest 分段驱动(§4.36):fixture 点名段(mutate/estimate/capacity)
    // 才进 stage_filter;不填 = 整挂点一段跑。
    if (const auto it = trigger_it->find("stage"); it != trigger_it->end() && it->is_string()) {
        hooks::middleware::Stage stage{};
        if (!hooks::middleware::ParseStage(it->get<std::string>(), stage)) {
            result.detail = "trigger.stage 不认识(只收 mutate/estimate/capacity/default): " +
                            it->get<std::string>();
            return result;
        }
        trigger.stage_filter = stage;
    }
    std::atomic<bool> cancelled{fixture.value("cancel", false)};
    if (cancelled.load()) {
        trigger.cancel = &cancelled;
    }

    // 链尾桩:fixture 给 terminal.adoptedPrompt 时回"宿主接纳后的下一帧"
    //(PostUser 一类挂点的 input 会带这帧);否则原样回 expected(观察用)。
    hooks::middleware::TerminalFn terminal = nullptr;
    if (const auto it = fixture.find("terminal"); it != fixture.end()) {
        nlohmann::json expected_terminal = *it;
        if (expected_terminal.is_object() && expected_terminal.contains("adoptedPrompt")) {
            const std::string adopted = expected_terminal.at("adoptedPrompt").get<std::string>();
            terminal = [adopted](const nlohmann::json& input) {
                nlohmann::json next = input.is_object() ? input : nlohmann::json::object();
                next["prompt"] = adopted;
                return next;
            };
        } else {
            terminal = [expected_terminal](const nlohmann::json&) { return expected_terminal; };
        }
    }

    const DispatchOutcome outcome = dispatcher.Dispatch(point, trigger, std::move(terminal), nullptr);

    result.ran = true;
    if (const auto expect_it = fixture.find("expect");
        expect_it != fixture.end() && expect_it->is_object() && !expect_it->empty()) {
        const std::vector<std::string> failures = CheckExpectation(*expect_it, outcome);
        result.pass = failures.empty();
        result.detail = failures.empty() ? std::string("断言全过")
                                         : JoinLines(failures);
    } else {
        // 没有 expect:只要不是 failed 就算过(冒烟档)。
        result.pass = outcome.kind != DispatchOutcome::Kind::Failed;
        result.detail = result.pass ? "冒烟过(无 expect)" : "dispatch 失败: " + outcome.error_code + " " +
                                                                outcome.error_detail;
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// 总入口
// ---------------------------------------------------------------------------

HookCheckReport RunHookPackageCheck(const std::filesystem::path& package_dir, HookCheckOptions options) {
    HookCheckReport report;
    report.package_dir = package_dir.string();
    auto fail_package = [&report](std::string why) {
        HookCheckReport::Check check;
        check.item = "package";
        check.pass = false;
        check.detail = std::move(why);
        report.checks.push_back(std::move(check));
        report.unverified.push_back("整包未验:包读不到,静态/fake/真实集成都没跑");
        return report;
    };

    std::error_code ec;
    if (!std::filesystem::is_directory(package_dir, ec) || ec) {
        return fail_package("包目录不存在: " + package_dir.string());
    }
    const std::filesystem::path manifest_path = package_dir / "hook.json";
    const auto manifest_text = ReadTextFile(manifest_path);
    if (!manifest_text.has_value()) {
        return fail_package("hook.json 读不到: " + manifest_path.string());
    }
    const nlohmann::json manifest =
        nlohmann::json::parse(*manifest_text, nullptr, /*allow_exceptions=*/false);
    if (manifest.is_discarded()) {
        return fail_package("hook.json 不是合法 JSON: " + manifest_path.string());
    }

    bool static_ok = true;
    auto add_check = [&report, &static_ok](std::string item, bool pass, std::string detail) {
        HookCheckReport::Check check;
        check.item = std::move(item);
        check.pass = pass;
        check.detail = std::move(detail);
        report.checks.push_back(std::move(check));
        if (!pass) {
            static_ok = false;
        }
    };

    // ---- 静态 1:清单 schema(entry 解析在装载器同款口径)。----
    // 摘要字段全部先验型再取值——坏型清单的路也要走到 ParseHookManifest
    // 的稳定错误,不在摘要处抛异常。
    const auto safe_string = [&manifest](const char* key) {
        const auto it = manifest.find(key);
        return it != manifest.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const std::string entry = safe_string("entry");
    auto entry_path = hooks::middleware::ResolveHookEntryPath(package_dir, entry);
    std::string script;
    if (!entry_path.has_value()) {
        add_check("manifest", false, entry_path.error());
    } else {
        const auto script_text = ReadTextFile(*entry_path);
        if (!script_text.has_value()) {
            add_check("manifest", false, "entry 脚本读不到: " + entry_path->string());
        } else {
            script = *script_text;
            int schema_version = 0;
            if (const auto sv = manifest.find("schemaVersion");
                sv != manifest.end() && sv->is_number_integer()) {
                schema_version = sv->get<int>();
            }
            std::size_t hook_count = 0;
            if (const auto hooks_it = manifest.find("hooks");
                hooks_it != manifest.end() && hooks_it->is_array()) {
                hook_count = hooks_it->size();
            }
            add_check("manifest", true, "schemaVersion=" + std::to_string(schema_version) + ",id=" +
                                             safe_string("id") + ",hooks=" + std::to_string(hook_count));
        }
    }

    std::vector<MiddlewareDefinition> defs;
    if (static_ok) {
        auto parsed = hooks::middleware::ParseHookManifest(manifest, hooks::middleware::SourceLayer::User,
                                                           "validate " + package_dir.string(), script);
        if (!parsed.has_value()) {
            add_check("manifest", false, parsed.error().code + ": " + parsed.error().message);
        } else {
            defs = std::move(*parsed);
        }
    }

    // ---- 静态 2:Lua 语法 + handler 对账(Load 全 entries 一把过)。----
    if (static_ok) {
        std::vector<std::string> entries;
        for (const auto& hook : manifest.value("hooks", nlohmann::json::array())) {
            const std::string handler = hook.value("handler", std::string());
            if (std::find(entries.begin(), entries.end(), handler) == entries.end()) {
                entries.push_back(handler);
            }
        }
        LuaHostState::Options probe;
        probe.script = script;
        probe.chunk_name = entry;
        probe.entries = std::move(entries);
        probe.profile = tools::LuaProfile::HookDefault();
        auto state = LuaHostState::Load(std::move(probe));
        if (!state.has_value()) {
            add_check("lua", false, state.error());
        } else {
            add_check("lua", true,
                      "语法过,handler 对账 " + std::to_string(probe.entries.size()) + " 项(" + entry + ")");
        }
    }

    // ---- 静态 3:能力申请词表(拼错的 capability 永远拿不到授权)。----
    if (static_ok) {
        static const std::set<std::string> kKnownCaps = {
            std::string(kHookCapHttp),   std::string(kHookCapFsRead), std::string(kHookCapFsWrite),
            std::string(kHookCapState),  std::string(kHookCapContext), std::string(kHookCapLog),
            std::string(kHookCapTools),
        };
        std::vector<std::string> unknown;
        bool requested_http = false;
        bool requested_tools = false;
        for (const MiddlewareDefinition& def : defs) {
            for (const std::string& cap : def.capabilities) {
                if (kKnownCaps.count(cap) == 0) {
                    unknown.push_back(def.Key() + ": " + cap);
                }
                if (cap == std::string(kHookCapHttp)) {
                    requested_http = true;
                }
                if (cap == std::string(kHookCapTools)) {
                    requested_tools = true;
                }
            }
        }
        if (!unknown.empty()) {
            add_check("capabilities", false, "不认识的能力申请: " + JoinLines(unknown));
        } else {
            add_check("capabilities", true,
                      defs.empty() ? "无能力申请" : "能力申请词表全认得(" + std::to_string(defs.size()) + " 条定义)");
        }
        if (requested_http) {
            report.unverified.push_back("真实 HTTP 服务:未验(校验只走 fake transport,不动网)");
        }
        if (requested_tools) {
            report.unverified.push_back("真实 MCP/工具服务:未验(校验只走 fixture 编排的 fake 工具)");
        }
    }

    // ---- 静态 4:依赖计划(发布裁决:同层冲突/循环/缺依赖/阶段倒置)。----
    if (static_ok) {
        MiddlewarePool::Options pool_options;
        pool_options.lua_factory = [](const hooks::middleware::LuaHandlerSpec& spec,
                                      const hooks::middleware::HandlerLimits& limits) {
            return MakeLuaHookHandler(spec, limits, nullptr);
        };
        MiddlewarePool pool(std::move(pool_options));
        hooks::middleware::AddBuiltinRequestSlots(pool);
        hooks::middleware::AddBuiltinGoalReviewSlot(pool);
        for (const MiddlewareDefinition& def : defs) {
            pool.AddDefinition(def);
        }
        auto published = pool.Publish();
        if (!published.has_value()) {
            add_check("plan", false, published.error().code + ": " + published.error().message);
        } else {
            report.plan = (*published)->DescribePlan();
            add_check("plan", true, "发布过,registryRevision=" +
                                        std::to_string((*published)->revision()) + ";计划见 plan 字段");
        }
    }

    report.static_pass = static_ok;

    // ---- fake 档:fixtures 试跑(静态挂了不跑,§8.2 "可编译不等于已验证",
    //      反过来静态都不过就没有跑的意义)。----
    if (options.run_fixtures && static_ok) {
        const std::filesystem::path fixtures_dir = package_dir / "fixtures";
        std::vector<std::filesystem::path> files;
        if (std::filesystem::exists(fixtures_dir, ec) && !ec) {
            for (const auto& file : std::filesystem::directory_iterator(fixtures_dir, ec)) {
                if (file.is_regular_file() && file.path().extension() == ".json") {
                    files.push_back(file.path());
                }
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const auto text = ReadTextFile(file);
            if (!text.has_value()) {
                HookCheckReport::Fixture fixture;
                fixture.name = file.filename().string();
                fixture.detail = "fixture 读不到";
                report.fixtures.push_back(std::move(fixture));
                continue;
            }
            const nlohmann::json fixture =
                nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
            if (fixture.is_discarded() || !fixture.is_object()) {
                HookCheckReport::Fixture entry_result;
                entry_result.name = file.filename().string();
                entry_result.detail = "fixture 不是合法 JSON object";
                report.fixtures.push_back(std::move(entry_result));
                continue;
            }
            std::string fallback_name = file.stem().string();
            HookCheckReport::Fixture run;
            try {
                run = RunSingleFixture(fixture, fallback_name, package_dir, options.fs_scratch_root, defs,
                                       manifest);
            } catch (const std::exception& e) {
                run.name = fallback_name;
                run.detail = std::string("fixture 处理异常: ") + e.what();
            } catch (...) {
                run.name = fallback_name;
                run.detail = "fixture 处理异常(未知)";
            }
            report.fixtures.push_back(std::move(run));
        }
        if (files.empty()) {
            report.unverified.push_back("fixtures:未验(包内没有用例;行为未跑)");
        }
    } else if (options.run_fixtures && !static_ok) {
        report.unverified.push_back("fixtures:未跑(静态检查未过)");
    }

    report.unverified.push_back("真实集成:未验(须把包装进会话,沿既有授权与真实宿主执行口观察)");
    return report;
}

bool HookCheckReport::fixtures_pass() const {
    if (fixtures.empty()) {
        return false;
    }
    for (const Fixture& fixture : fixtures) {
        if (!fixture.ran || !fixture.pass) {
            return false;
        }
    }
    return true;
}

int HookCheckReport::exit_code() const {
    // 包读不到(package 检查挂)退 2;静态或 fixture 有 fail 退 1;全过 0。
    for (const Check& check : checks) {
        if (check.item == "package" && !check.pass) {
            return 2;
        }
    }
    if (!static_pass) {
        return 1;
    }
    for (const Fixture& fixture : fixtures) {
        if (!fixture.ran || !fixture.pass) {
            return 1;
        }
    }
    return 0;
}

nlohmann::json HookCheckReport::ToJson() const {
    nlohmann::json out;
    out["schema"] = 1;
    out["tool"] = "lubancode hook validate/test";
    out["packageDir"] = package_dir;
    nlohmann::json checks_json = nlohmann::json::array();
    for (const Check& check : checks) {
        checks_json.push_back(nlohmann::json{{"item", check.item},
                                             {"pass", check.pass},
                                             {"detail", check.detail}});
    }
    out["checks"] = std::move(checks_json);
    out["staticPass"] = static_pass;
    if (!plan.is_null()) {
        out["plan"] = plan;
    }
    nlohmann::json fixtures_json = nlohmann::json::array();
    for (const Fixture& fixture : fixtures) {
        fixtures_json.push_back(nlohmann::json{{"name", fixture.name},
                                               {"ran", fixture.ran},
                                               {"pass", fixture.pass},
                                               {"detail", fixture.detail}});
    }
    out["fixtures"] = std::move(fixtures_json);
    out["fixturesPass"] = fixtures.empty() ? false : fixtures_pass();
    out["unverified"] = unverified;
    out["exitCode"] = exit_code();
    return out;
}

}  // namespace lubancode::runtime
