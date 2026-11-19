// SDK-1:直接消费中立工具装配,核工具面/审批类别/宿主身份注入。
// 后端六口转发、替换与在途寿命由 unit.runtime.backend_stack 直接覆盖。
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "fake_http_server.hpp"
#include "runtime/assembly/builtin_tools.hpp"

namespace {

namespace assembly = lubancode::runtime::assembly;
using lubancode::config::SearchConfig;
using lubancode::tools::ApprovalClass;
using lubancode::tools::ToolRegistry;

std::vector<std::string> Names(const ToolRegistry& registry) {
    std::vector<std::string> names;
    for (const auto& tool : registry.All()) {
        names.push_back(tool->name());
    }
    return names;
}

}  // namespace

TEST_CASE("shared assembly: base tools retain names order and approval classes") {
    auto registry = assembly::BuildBaseToolRegistry({}, {}, "test-host/1");
    const std::vector<std::string> expected{
        "read_file", "run_command", "background_output", "stop_background",
        "write_file", "edit_file", "search", "skill", "web_fetch",
    };
    CHECK(Names(registry) == expected);

    const std::vector<std::pair<std::string, ApprovalClass>> approvals{
        {"run_command", ApprovalClass::Command},
        {"stop_background", ApprovalClass::External},
        {"write_file", ApprovalClass::FileEdit},
        {"edit_file", ApprovalClass::FileEdit},
    };
    for (const auto& [name, approval] : approvals) {
        CAPTURE(name);
        const auto* tool = registry.Find(name);
        REQUIRE(tool != nullptr);
        CHECK(tool->needs_confirm());
        CHECK(tool->approval_class() == approval);
        const auto* registration = registry.RegistrationOf(name);
        REQUIRE(registration != nullptr);
        CHECK(registration->source_kind == lubancode::tools::ToolSourceKind::Builtin);
        CHECK(registration->effect_class == tool->effect_class());
    }
    for (const char* name : {"read_file", "search", "skill", "web_fetch", "background_output"}) {
        CAPTURE(name);
        const auto* tool = registry.Find(name);
        REQUIRE(tool != nullptr);
        CHECK_FALSE(tool->needs_confirm());
    }
}

TEST_CASE("shared assembly: exploration never gains command or file mutation tools") {
    for (const SearchConfig search : {SearchConfig{}, SearchConfig{"tavily", "FAKE_SEARCH_KEY"}}) {
        auto registry = assembly::BuildExploreToolRegistry(search, "test-host/1");
        std::vector<std::string> expected{"read_file", "search", "web_fetch"};
        if (search.Configured()) expected.push_back("web_search");
        CHECK(Names(registry) == expected);
        for (const auto& tool : registry.All()) {
            CHECK_FALSE(tool->needs_confirm());
            CHECK(tool->approval_class() == ApprovalClass::None);
        }
        for (const char* absent : {"run_command", "write_file", "edit_file", "stop_background", "skill"}) {
            CHECK(registry.Find(absent) == nullptr);
        }
    }
}

TEST_CASE("shared assembly: web search requires both provider and credential") {
    for (const SearchConfig search : {SearchConfig{}, SearchConfig{"tavily", ""},
                                     SearchConfig{"", "FAKE_SEARCH_KEY"},
                                     SearchConfig{"tavily", "FAKE_SEARCH_KEY"}}) {
        auto base = assembly::BuildBaseToolRegistry({}, search, "test-host/1");
        auto explore = assembly::BuildExploreToolRegistry(search, "test-host/1");
        const bool enabled = !search.provider.empty() && !search.api_key.empty();
        CHECK((base.Find("web_search") != nullptr) == enabled);
        CHECK((explore.Find("web_search") != nullptr) == enabled);
    }
}

TEST_CASE("shared assembly: skill list is captured by value for each registry") {
    std::vector<lubancode::tools::SkillMeta> skills(1);
    skills.front().name = "fixture-skill";
    skills.front().description = "only this declared skill";
    auto first = assembly::BuildBaseToolRegistry(skills, {}, "test-host/1");
    skills.front().name = "later-skill";
    auto second = assembly::BuildBaseToolRegistry(skills, {}, "test-host/1");
    const auto* first_skill = first.Find("skill");
    const auto* second_skill = second.Find("skill");
    REQUIRE(first_skill != nullptr);
    REQUIRE(second_skill != nullptr);
    CHECK(first_skill != second_skill);
    CHECK(first_skill->input_schema().dump().find("fixture-skill") != std::string::npos);
    CHECK(first_skill->input_schema().dump().find("later-skill") == std::string::npos);
    CHECK(second_skill->input_schema().dump().find("later-skill") != std::string::npos);
}

TEST_CASE("shared assembly: explicit host user agent reaches both web fetch registries") {
    lubancode::test_support::FakeHttpServer server;
    REQUIRE(server.port() > 0);
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/fixture";
    const std::string user_agent = "embedding-host/FAKE_VERSION";
    auto base = assembly::BuildBaseToolRegistry({}, {}, user_agent);
    auto explore = assembly::BuildExploreToolRegistry({}, user_agent);
    for (ToolRegistry* registry : {&base, &explore}) {
        lubancode::test_support::FakeHttpResponse response;
        response.headers.emplace_back("Content-Type", "text/plain");
        response.body = "fixture output";
        server.Enqueue(std::move(response));
        auto* fetch = registry->Find("web_fetch");
        REQUIRE(fetch != nullptr);
        const auto result = fetch->execute({{"url", url}});
        CHECK_FALSE(result.is_error);
    }
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    for (const auto& request : requests) {
        const auto header = std::find_if(request.headers.begin(), request.headers.end(),
                                        [](const auto& value) { return value.first == "user-agent"; });
        REQUIRE(header != request.headers.end());
        CHECK(header->second == user_agent);
    }
}
