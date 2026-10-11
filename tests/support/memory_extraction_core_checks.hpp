#pragma once
#include <doctest/doctest.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#if defined(LUBANCORE_TEST_MODEL_SAMPLING) || defined(LUBANCORE_TEST_JOB_POST_SDK)
#include <lubancore/core.hpp>
#include "sdk/adapters.hpp"
#endif
#include "agent/memory_extraction_test_port.hpp"
#include "api/usage_aggregation.hpp"
#include "config/config.hpp"
#include "fake_http_server.hpp"
#include "platform/text_encoding.hpp"
#include "sample_lifetime_checks.hpp"

namespace memory_extraction_fixture {
namespace agent = lubancode::agent;
namespace api = lubancode::api;
namespace core = agent::memory_extraction;
using Port = agent::testing::MemoryExtractionTestPort;
inline void Mark(const char* module,const char* path) {
    std::cout << "[memory-extraction-core-path] " << module << ' ' << path << '\n';
}
inline std::string ValidBody() { return R"({"task_type":"code","summary":"shared summary","retrieval_terms":["owner"],"candidates":[{"kind":"fact","title":"owned","content":"verified","confidence":"verified"}]})"; }
inline api::Message Text(api::Role role,std::string text) {
    api::Message message; message.role=role; message.content.push_back(api::TextBlock{std::move(text)}); return message;
}
struct Backend final : api::Backend {
    std::string body=ValidBody(),reason="end_turn";
    api::MessageDone done;
    api::Request request;
    int calls=0; bool fail=false,cancelled=false;
    std::expected<void,api::Error> send_stream(const api::Request& value,
        const std::function<void(const api::StreamEvent&)>& emit,const std::atomic<bool>* cancel) override {
        ++calls; request=value;
        if (cancelled) { CHECK(cancel!=nullptr); CHECK(cancel->load()); return std::unexpected(api::Error{api::ErrorKind::Cancelled,"host stop",0}); }
        emit(api::MessageStart{"actual-provider","model"}); emit(api::TextDelta{body});
        done.stop_reason=reason; emit(done);
        if (fail) return std::unexpected(api::Error{api::ErrorKind::Network,"partial failure",0});
        return {};
    }
};
struct Recorder : sample_lifetime_fixture::Recorder {
    bool read=false,write=false; int epoch=-1; bool append=false; std::string anomaly,provider;
    void OnUsageRecorded(const std::string& id,const api::Usage& value,bool reported,
        const std::string& provider_id,int cache_epoch,bool append_only,bool read_reported,
        bool write_reported,const std::string& usage_anomaly) override {
        sample_lifetime_fixture::Recorder::OnUsageRecorded(id,value,reported,provider_id,cache_epoch,append_only,read_reported,write_reported,usage_anomaly);
        read=read_reported; write=write_reported; epoch=cache_epoch; append=append_only;
        anomaly=usage_anomaly; provider=provider_id;
    }
};
inline void Parser(const Port& port,const char* module) {
    for (const auto& body : {ValidBody(),"```json\n"+ValidBody()+"\n```","说明\n"+ValidBody()}) {
        const auto result=port.parse(body); REQUIRE(result.has_value()); CHECK(result->summary=="shared summary");
        REQUIRE(result->candidates.size()==1); CHECK(result->candidates[0].content=="verified"); CHECK(result->candidates[0].occurred_at.empty());
    }
    for (const auto& body : {ValidBody()+ValidBody(),std::string("{\"summary\":"),std::string("{} trailing {" )}) {
        const auto result=port.parse(body); REQUIRE_FALSE(result.has_value()); CHECK(result.error().code==core::ExtractionErrorCode::SyntaxInvalid);
    }
    const auto invalid=port.parse(std::string(1,static_cast<char>(0xff))); REQUIRE_FALSE(invalid.has_value()); CHECK(invalid.error().code==core::ExtractionErrorCode::Utf8Invalid);
    auto body=nlohmann::json::parse(ValidBody()); body["summary"]=17;
    const auto bad_type=port.parse(body.dump()); REQUIRE_FALSE(bad_type.has_value()); CHECK(bad_type.error().code==core::ExtractionErrorCode::SchemaInvalid); CHECK(bad_type.error().field_path=="summary");
    body=nlohmann::json::parse(ValidBody()); auto candidate=body["candidates"][0];
    body["candidates"]={candidate,candidate,candidate,candidate}; const auto limited=port.parse(body.dump()); REQUIRE(limited.has_value()); CHECK(limited->candidates.size()==3);
    body["candidates"][0]["content"]=std::string(8193,'x'); const auto skipped=port.parse(body.dump()); REQUIRE(skipped.has_value()); CHECK(skipped->candidates.size()==3);
    Mark(module,"parser");
}
inline void Transcript(const Port& port,const char* module) {
    std::vector<api::Message> messages{Text(api::Role::User,std::string(3000,'u')),
        Text(api::Role::Assistant,"intermediate-secret"),Text(api::Role::Assistant,std::string(4000,'a'))};
    const auto out=port.transcript(messages,8192); CHECK(out.size()<=8192); CHECK(out.find("intermediate-secret")==std::string::npos);
    CHECK(out.find(std::string(2049,'u'))==std::string::npos); CHECK(out.find(std::string(3073,'a'))==std::string::npos);
    messages={Text(api::Role::User,"中文材料中文材料")};
    for (std::size_t cap=0;cap!=60;++cap) { const auto clipped=port.transcript(messages,cap); CHECK(clipped.size()<=cap); CHECK(lubancode::platform::IsValidUtf8(clipped)); }
    CHECK(port.classify("修复 bug",{"edit_file"})=="code"); CHECK(port.classify("写一份 README 文档",{})=="docs");
    messages={Text(api::Role::User,"inspect")};
    for (int i=0;i!=8;++i) {
        api::Message call; call.role=api::Role::Assistant; api::ToolUseBlock use;
        use.id="tool-"+std::to_string(i); use.name="read_file";
        use.input={{"path","file-"+std::to_string(i)},{"patch","private-patch-body"},{"content","private-file-body"}};
        call.content.push_back(use); messages.push_back(std::move(call));
        api::Message reply; reply.role=api::Role::User; api::ToolResultBlock result;
        result.tool_use_id=use.id; result.content="result-"+std::to_string(i)+std::string(500,'z'); reply.content.push_back(std::move(result)); messages.push_back(std::move(reply));
    }
    const auto tools=port.transcript(messages,8192); CHECK(tools.size()<=8192);
    CHECK(tools.find("private-patch-body")==std::string::npos); CHECK(tools.find("private-file-body")==std::string::npos);
    CHECK(tools.find("result-0")==std::string::npos); CHECK(tools.find("result-7")!=std::string::npos);
    CHECK(tools.find(std::string(241,'z'))==std::string::npos);
    Mark(module,"transcript");
}
inline void Prompt(const Port& port,const char* module) {
    CHECK_FALSE(port.prompt("","code").empty()); CHECK(port.prompt("","unknown-type")==port.prompt("","other"));
    const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto dir=std::filesystem::temp_directory_path()/("memory-core-prompt-"+unique);
    std::filesystem::create_directories(dir/"features");
    struct Cleanup { std::filesystem::path dir; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(dir,ec);} } cleanup{dir};
    std::ofstream(dir/"features/memory-summary-base.md") << "explicit base";
    std::ofstream(dir/"features/memory-summary-code.md") << "explicit code";
    CHECK(port.prompt(dir.string(),"code")=="explicit base\n\nexplicit code");
    std::ofstream(dir/"features/memory-summary-unregistered.md") << "must not read arbitrary module";
    CHECK(port.prompt(dir.string(),"unregistered").find("must not read arbitrary module")==std::string::npos);
    Mark(module,"prompt");
}
inline void Request(const Port& port,const char* module) {
    Backend backend; backend.done.usage.input_tokens=11; backend.done.usage.output_tokens=7;
    agent::BackgroundCallAccounting accounting; accounting.usage.input_tokens=3;
    Recorder recorder;
    const auto result=port.run(backend,"actual-model","explicit prompt","frozen material",0,"future-level",&accounting,&recorder,nullptr);
    REQUIRE(result.has_value()); CHECK(backend.calls==1); CHECK(backend.request.model=="actual-model");
    CHECK(backend.request.system=="explicit prompt"); CHECK(backend.request.reasoning_effort=="future-level");
    CHECK(backend.request.max_tokens==4096); CHECK(backend.request.tools.empty());
    REQUIRE(backend.request.messages.size()==1); CHECK(std::get<api::TextBlock>(backend.request.messages[0].content[0]).text=="frozen material");
    CHECK(accounting.usage.input_tokens==14); CHECK(accounting.usage.output_tokens==7); CHECK(accounting.usage_reported);
    CHECK(accounting.usage_coverage.samples == 1);
    CHECK_FALSE(api::usage_aggregation::Exact(accounting.usage_coverage, ::lubancore::usage::v1::Field::Input));
    CHECK(recorder.completed==1); CHECK(recorder.failed==0); CHECK(recorder.reported); CHECK(recorder.provider=="actual-provider");
    CHECK(port.schema().at("type")=="object");
    agent::BackgroundCallAccounting bounded;
    bounded.usage.output_reasoning_tokens = std::numeric_limits<std::int64_t>::max();
    Backend overflow;
    overflow.done.usage.input_tokens = 3;
    overflow.done.usage.output_tokens = 5;
    overflow.done.usage.cache_read_tokens = 7;
    overflow.done.usage.cache_creation_tokens = 11;
    overflow.done.usage.output_reasoning_tokens = 1;
    REQUIRE(port.run(overflow,"actual-model","explicit prompt","frozen material",0,"",&bounded,nullptr,nullptr).has_value());
    CHECK(bounded.usage.input_tokens == 3);
    CHECK(bounded.usage.output_tokens == 5);
    CHECK(bounded.usage.cache_read_tokens == 7);
    CHECK(bounded.usage.cache_creation_tokens == 11);
    CHECK(bounded.usage.output_reasoning_tokens == std::numeric_limits<std::int64_t>::max());
    const auto& reasoning = bounded.usage_coverage.fields[4];
    CHECK(reasoning.arithmetic_overflow);
    CHECK(reasoning.omitted == 1);
    overflow.fail = true;
    const auto partial = port.run(overflow,"actual-model","explicit prompt","frozen material",0,"",&bounded,nullptr,nullptr);
    REQUIRE_FALSE(partial.has_value());
    CHECK(partial.error().code == core::ExtractionErrorCode::TransportFailed);
    CHECK(bounded.usage.input_tokens == 6); CHECK(bounded.usage.output_tokens == 10);
    CHECK(bounded.usage.cache_read_tokens == 14); CHECK(bounded.usage.cache_creation_tokens == 22);
    CHECK(bounded.usage.output_reasoning_tokens == std::numeric_limits<std::int64_t>::max());
    CHECK(bounded.usage_coverage.fields[4].omitted == 2);
    CHECK(bounded.usage_coverage.samples == 2);
    Mark(module,"request");
}
inline void Usage(const Port& port,const char* module) {
    for (int scenario=0;scenario!=7;++scenario) {
        Backend backend; Recorder recorder; agent::SampleOptions options; options.boundary_recorder=&recorder;
        if (scenario>=1) backend.done.usage_reported=true;
        if (scenario==2) backend.done.cache_read_reported=true;
        if (scenario==3) backend.done.cache_creation_reported=true;
        if (scenario==4) { backend.done.usage={11,7,13,17,3}; backend.done.cache_read_reported=true; backend.done.cache_creation_reported=true; backend.done.usage_anomaly="provider inconsistency"; }
        if (scenario==5) { backend.done.usage_reported=false; backend.done.usage.cache_read_tokens=9; }
        if (scenario==6) { backend.done.cache_creation_reported=true; backend.fail=true; }
        const auto result=port.sample(backend,{},options);
        CHECK(result.ok==(scenario!=6)); CHECK(result.usage_reported==(scenario!=0)); CHECK(recorder.reported==result.usage_reported);
        CHECK(result.cache_read_reported==backend.done.cache_read_reported); CHECK(result.cache_creation_reported==backend.done.cache_creation_reported);
        CHECK(recorder.read==result.cache_read_reported); CHECK(recorder.write==result.cache_creation_reported);
        CHECK(result.usage_anomaly==backend.done.usage_anomaly); CHECK(recorder.anomaly==result.usage_anomaly);
        CHECK(recorder.epoch==0); CHECK(recorder.append); CHECK(recorder.owners==1);
        CHECK(result.usage.input_tokens==backend.done.usage.input_tokens); CHECK(result.usage.output_tokens==backend.done.usage.output_tokens);
        CHECK(result.usage.cache_read_tokens==backend.done.usage.cache_read_tokens); CHECK(result.usage.cache_creation_tokens==backend.done.usage.cache_creation_tokens);
        CHECK(result.usage.output_reasoning_tokens==backend.done.usage.output_reasoning_tokens);
        if (scenario==6) { CHECK(result.text==ValidBody()); CHECK(recorder.failed==1); CHECK(recorder.completed==0); }
    }
#if defined(LUBANCORE_TEST_MODEL_SAMPLING) || defined(LUBANCORE_TEST_JOB_POST_SDK)
    struct PublicBackend final : lubancore::Backend {
        bool report=true;
        lubancore::Result<lubancore::ModelReply> Generate(const lubancore::ModelRequest&,lubancore::Cancellation) override {
            lubancore::ModelReply reply; reply.text=ValidBody(); if(report) reply.usage=lubancore::Usage{}; return reply;
        }
    };
    const auto public_backend=std::make_shared<PublicBackend>(); auto adapter=lubancore::detail::AdaptBackend(public_backend);
    CHECK(port.sample(*adapter,{},{}).usage_reported); public_backend->report=false; CHECK_FALSE(port.sample(*adapter,{},{}).usage_reported);
#endif
    Mark(module,"usage");
}
inline void Finish(const Port& port,const char* module) {
    agent::SampleResult sampled; sampled.ok=true; sampled.text=ValidBody(); sampled.provider_response_id="actual-id";
    for(const auto* reason : {"length","max_tokens","max_output_tokens"}) { sampled.stop_reason=reason; const auto result=port.finish(sampled); REQUIRE_FALSE(result.has_value()); CHECK(result.error().code==core::ExtractionErrorCode::OutputTruncated); CHECK(result.error().request_id=="actual-id"); }
    sampled.stop_reason="future-stop"; CHECK(port.finish(sampled).has_value()); sampled.text.clear(); CHECK(port.finish(sampled).error().code==core::ExtractionErrorCode::EmptyOutput);
    sampled.ok=false; sampled.error={api::ErrorKind::Cancelled,"timeout",0}; sampled.error.api_code="local_deadline";
    CHECK(port.finish(sampled).error().code==core::ExtractionErrorCode::DeadlineTimeout);
    Backend backend; backend.cancelled=true; std::atomic<bool> cancel{true}; Recorder recorder;
    const auto result=port.run(backend,"model","prompt","material",0,"",nullptr,&recorder,&cancel);
    REQUIRE_FALSE(result.has_value()); CHECK(result.error().code==core::ExtractionErrorCode::TransportFailed); CHECK(recorder.cancelled==1);
    Mark(module,"finish");
}
inline void NativeConnection(const Port& port,const char* module) {
    using Server=lubancode::test_support::FakeHttpServer;
    Server server(Server::ThreadMode::Owned); REQUIRE(server.port()>0);
    const auto event=[](const nlohmann::json& value){return "data: "+value.dump()+"\n\n";};
    std::string wire=event({{"type","message_start"},{"message",{{"id","native-memory"},{"model","model"},{"content",nlohmann::json::array()},{"usage",{{"input_tokens",11},{"cache_creation_input_tokens",17},{"cache_read_input_tokens",13},{"output_tokens",0}}}}}});
    wire+=event({{"type","content_block_start"},{"index",0},{"content_block",{{"type","text"},{"text",""}}}});
    wire+=event({{"type","content_block_delta"},{"index",0},{"delta",{{"type","text_delta"},{"text",ValidBody()}}}});
    wire+=event({{"type","content_block_stop"},{"index",0}});
    wire+=event({{"type","message_delta"},{"delta",{{"stop_reason","end_turn"}}},{"usage",{{"output_tokens",7}}}});
    wire+=event({{"type","message_stop"}});
    lubancode::test_support::FakeHttpResponse response; response.headers={{"Content-Type","text/event-stream"}}; response.body=wire; server.Enqueue(std::move(response));
    lubancode::config::Config config; config.wire=lubancode::config::Wire::Anthropic; config.base_url="http://127.0.0.1:"+std::to_string(server.port()); config.auth_token="FAKE_MEMORY_CORE";
    config.connect_timeout_ms=2000; config.stream_idle_timeout_secs=3; config.request_hard_timeout_secs=5;
    auto backend=port.connection(config); REQUIRE(backend!=nullptr); Recorder recorder; agent::SampleOptions options; options.timeout_secs=5; options.boundary_recorder=&recorder;
    agent::SampleRequest request; request.model="model"; request.max_tokens=4096; request.messages.push_back(Text(api::Role::User,"material"));
    const auto sampled=port.sample(*backend,request,options); REQUIRE(sampled.ok); CHECK(port.finish(sampled).has_value());
    CHECK(sampled.usage.input_tokens==11); CHECK(sampled.usage.output_tokens==7); CHECK(sampled.usage.cache_read_tokens==13); CHECK(sampled.usage.cache_creation_tokens==17); CHECK(sampled.usage.output_reasoning_tokens==0);
    CHECK(sampled.usage_reported); CHECK(sampled.cache_read_reported); CHECK(sampled.cache_creation_reported); CHECK(recorder.read); CHECK(recorder.write); CHECK(recorder.provider=="native-memory");
    std::string responses_wire=event({{"type","response.created"},{"response",{{"id","responses-memory"},{"model","model"},{"status","in_progress"}}}});
    responses_wire+=event({{"type","response.output_text.delta"},{"item_id","msg_memory"},{"output_index",0},{"content_index",0},{"delta",ValidBody()}});
    responses_wire+=event({{"type","response.completed"},{"response",{{"id","responses-memory"},{"status","completed"},{"output",nlohmann::json::array()},
        {"usage",{{"input_tokens",24},{"output_tokens",7},{"total_tokens",31},{"input_tokens_details",{{"cached_tokens",13}}},{"output_tokens_details",{{"reasoning_tokens",3}}}}}}}});
    lubancode::test_support::FakeHttpResponse responses_reply; responses_reply.headers={{"Content-Type","text/event-stream"}}; responses_reply.body=responses_wire; server.Enqueue(std::move(responses_reply));
    config.wire=lubancode::config::Wire::Responses; auto responses_backend=port.connection(config); REQUIRE(responses_backend!=nullptr);
    const auto responses_sample=port.sample(*responses_backend,request,{}); REQUIRE(responses_sample.ok); CHECK(port.finish(responses_sample).has_value());
    CHECK(responses_sample.usage.input_tokens==11); CHECK(responses_sample.usage.cache_read_tokens==13); CHECK(responses_sample.usage.output_tokens==7); CHECK(responses_sample.usage.output_reasoning_tokens==3);
    CHECK(responses_sample.usage.cache_creation_tokens==0); CHECK_FALSE(responses_sample.cache_creation_reported); CHECK(responses_sample.cache_read_reported);
    CHECK(api::TotalInputTokens(responses_sample.usage)==24); CHECK(responses_sample.usage.output_tokens==7);
    REQUIRE(sampled.usage_observation.has_value());
    REQUIRE(responses_sample.usage_observation.has_value());
    CHECK_FALSE(sampled.usage_observation->raw_fields.empty());
    CHECK_FALSE(responses_sample.usage_observation->raw_fields.empty());
    std::string chat_wire = event({{"id", "chat-memory"}, {"model", "model"},
        {"choices", nlohmann::json::array({{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", ValidBody()}}},
                                           {"finish_reason", nullptr}}})}});
    chat_wire += event({{"id", "chat-memory"}, {"choices", nlohmann::json::array({{{"index", 0}, {"delta", nlohmann::json::object()}, {"finish_reason", "stop"}}})},
        {"usage", {{"prompt_tokens", 24}, {"completion_tokens", 7}, {"total_tokens", 31},
                   {"prompt_tokens_details", {{"cached_tokens", 13}}}, {"completion_tokens_details", {{"reasoning_tokens", 3}}}}}});
    chat_wire += "data: [DONE]\n\n";
    lubancode::test_support::FakeHttpResponse chat_reply;
    chat_reply.headers = {{"Content-Type", "text/event-stream"}}; chat_reply.body = chat_wire;
    server.Enqueue(std::move(chat_reply));
    config.wire = lubancode::config::Wire::ChatCompletions;
    auto chat_backend = port.connection(config); REQUIRE(chat_backend != nullptr);
    const auto chat_sample = port.sample(*chat_backend, request, {});
    REQUIRE(chat_sample.ok); CHECK(port.finish(chat_sample).has_value());
    CHECK(chat_sample.provider_response_id == "chat-memory");
    CHECK(chat_sample.usage.input_tokens == 11); CHECK(chat_sample.usage.output_tokens == 7);
    CHECK(chat_sample.usage.cache_read_tokens == 13); CHECK(chat_sample.usage.output_reasoning_tokens == 3);
    REQUIRE(chat_sample.usage_observation.has_value());
    CHECK_FALSE(chat_sample.usage_observation->raw_fields.empty());

    const auto gemini_wire = event({{"responseId", "gemini-memory"}, {"modelVersion", "model"},
        {"candidates", nlohmann::json::array({{{"index", 0}, {"finishReason", "STOP"},
            {"content", {{"role", "model"}, {"parts", nlohmann::json::array({{{"text", ValidBody()}}})}}}}})},
        {"usageMetadata", {{"promptTokenCount", 24}, {"cachedContentTokenCount", 13},
                            {"candidatesTokenCount", 4}, {"thoughtsTokenCount", 3}, {"totalTokenCount", 31}}}});
    lubancode::test_support::FakeHttpResponse gemini_reply;
    gemini_reply.headers = {{"Content-Type", "text/event-stream"}}; gemini_reply.body = gemini_wire;
    server.Enqueue(std::move(gemini_reply));
    config.wire = lubancode::config::Wire::GoogleGenerateContent;
    auto gemini_backend = port.connection(config); REQUIRE(gemini_backend != nullptr);
    const auto gemini_sample = port.sample(*gemini_backend, request, {});
    REQUIRE(gemini_sample.ok); CHECK(port.finish(gemini_sample).has_value());
    CHECK(gemini_sample.provider_response_id == "gemini-memory");
    CHECK(gemini_sample.usage.input_tokens == 11); CHECK(gemini_sample.usage.output_tokens == 7);
    CHECK(gemini_sample.usage.cache_read_tokens == 13); CHECK(gemini_sample.usage.output_reasoning_tokens == 3);
    REQUIRE(gemini_sample.usage_observation.has_value());
    CHECK_FALSE(gemini_sample.usage_observation->raw_fields.empty());
    server.StopAndJoin(); CHECK(server.owned_threads_quiescent()); REQUIRE(server.requests().size()==4); CHECK(server.requests()[0].method=="POST"); CHECK(server.requests()[1].method=="POST");
    CHECK(server.requests()[2].method == "POST"); CHECK(server.requests()[3].method == "POST");
    Mark(module,"native-connection");
}

inline void NativeConnectionFaults(const Port& port, const char* module) {
    // Only delegation and fault injection live in this wrapper. All facts and
    // physical transport outcomes come from the actual linked Connection.
    struct AfterFacts final : api::Backend {
        std::unique_ptr<api::Backend> connection;
        std::atomic<bool>* stop = nullptr;
        int mode = 0, calls = 0, facts = 0, terminals = 0;
        std::expected<void, api::Error> send_stream(const api::Request& request,
            const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel) override {
            ++calls;
            return connection->send_stream(request, [&](const api::StreamEvent& event) {
                if (mode >= 5 && std::holds_alternative<api::ProviderResponseIdentity>(event)) {
                    // The actual Connection parsed its facts; the sampling
                    // owner has received neither identity nor usage yet.
                    if (mode == 6) throw std::bad_alloc{};
                    if (mode == 7) throw 19;
                    throw std::runtime_error("actual Connection identity callback fault");
                }
                const bool observed = std::holds_alternative<api::UsageSnapshot>(event);
                if (observed) ++facts;
                if (std::holds_alternative<api::MessageDone>(event)) ++terminals;
                emit(event); // Actual sampling owner sees the numbers before the injected fault.
                if (!observed || mode == 0 || mode >= 5) return;
                stop->store(true);
                if (mode == 2) throw std::runtime_error("actual Connection callback fault");
                if (mode == 3) throw std::bad_alloc{};
                if (mode == 4) throw 19;
            }, cancel);
        }
    };
    using Wire = lubancode::config::Wire;
    using Server = lubancode::test_support::FakeHttpServer;
    const auto event = [](const nlohmann::json& value) { return "data: " + value.dump() + "\n\n"; };
    for (const auto wire : {Wire::Anthropic, Wire::Responses, Wire::ChatCompletions, Wire::GoogleGenerateContent})
        for (int mode = 0; mode < 8; ++mode) {
            INFO(static_cast<int>(wire)); INFO(mode);
            Server server(Server::ThreadMode::Owned); REQUIRE(server.port() > 0);
            const std::string response_id = "native-fault-" + std::to_string(static_cast<int>(wire)) + "-" + std::to_string(mode);
            nlohmann::json prefix;
            if (wire == Wire::Anthropic) {
                prefix = {{"type", "message_start"}, {"message", {{"id", response_id}, {"model", "model"},
                    {"content", nlohmann::json::array()}, {"usage", {{"input_tokens", 11}, {"output_tokens", 7},
                    {"cache_read_input_tokens", 13}, {"cache_creation_input_tokens", 17}}}}}};
            } else if (wire == Wire::Responses) {
                prefix = {{"type", "response.created"}, {"response", {{"id", response_id}, {"model", "model"},
                    {"status", "in_progress"}, {"usage", {{"input_tokens", 41}, {"output_tokens", 7}, {"total_tokens", 48},
                    {"input_tokens_details", {{"cached_tokens", 13}, {"cache_write_tokens", 17}}},
                    {"output_tokens_details", {{"reasoning_tokens", 3}}}}}}}};
            } else if (wire == Wire::ChatCompletions) {
                prefix = {{"id", response_id}, {"model", "model"}, {"choices", nlohmann::json::array()},
                    {"usage", {{"prompt_tokens", 41}, {"completion_tokens", 7}, {"total_tokens", 48},
                    {"prompt_tokens_details", {{"cached_tokens", 13}, {"cache_write_tokens", 17}}},
                    {"completion_tokens_details", {{"reasoning_tokens", 3}}}}}};
            } else {
                prefix = {{"responseId", response_id}, {"modelVersion", "model"},
                    {"usageMetadata", {{"promptTokenCount", 24}, {"cachedContentTokenCount", 13},
                    {"candidatesTokenCount", 4}, {"thoughtsTokenCount", 3}, {"totalTokenCount", 31}}}};
            }
            nlohmann::json failure{{"type", "error"}, {"error", {{"message", "native source failure"},
                {"code", "native.source.failure"}, {"type", "native.source.failure"}}}};
            const auto prefix_bytes = event(prefix);
            lubancode::test_support::FakeHttpResponse response;
            response.headers = {{"Content-Type", "text/event-stream"}};
            response.body = prefix_bytes + event(failure);
            if (mode == 1) response.stall_after_body_bytes = prefix_bytes.size();
            server.Enqueue(std::move(response));
            lubancode::config::Config config;
            config.wire = wire; config.base_url = "http://127.0.0.1:" + std::to_string(server.port());
            config.auth_token = "FAKE_NATIVE_FAULT"; config.connect_timeout_ms = 2000;
            config.stream_idle_timeout_secs = 3; config.request_hard_timeout_secs = 5;
            std::atomic<bool> stop{false};
            AfterFacts backend; backend.connection = port.connection(config); REQUIRE(backend.connection != nullptr);
            backend.stop = &stop; backend.mode = mode;
            Recorder recorder;
            agent::SampleOptions options; options.cancel = &stop;
            options.cancel_source = agent::OutputCancelSource::Internal; options.boundary_recorder = &recorder;
            agent::SampleRequest request; request.model = "model"; request.max_tokens = 4096;
            request.messages.push_back(Text(api::Role::User, "material"));
            const auto sampled = port.sample(backend, request, options);
            CHECK_FALSE(sampled.ok); CHECK(sampled.stop_reason.empty()); CHECK(sampled.text.empty());
            if (mode >= 5) CHECK(sampled.provider_response_id.empty());
            else CHECK(sampled.provider_response_id == response_id);
            CHECK(sampled.usage.input_tokens == 11); CHECK(sampled.usage.output_tokens == 7);
            CHECK(sampled.usage.cache_read_tokens == 13);
            CHECK(sampled.usage.cache_creation_tokens == (wire == Wire::GoogleGenerateContent ? 0 : 17));
            CHECK(sampled.usage.output_reasoning_tokens == (wire == Wire::Anthropic ? 0 : 3));
            if (mode >= 5) CHECK_FALSE(sampled.usage_observation.has_value());
            else {
                REQUIRE(sampled.usage_observation.has_value());
                CHECK_FALSE(sampled.usage_observation->raw_fields.empty());
            }
            CHECK(backend.calls == 1); CHECK(backend.facts == 1); CHECK(backend.terminals == 0);
            CHECK(recorder.owners == 1); CHECK(recorder.completed == 0);
            if (mode == 1) {
                CHECK(sampled.error.kind == api::ErrorKind::Cancelled); CHECK(recorder.cancelled == 1);
            } else {
                CHECK(sampled.error.kind == api::ErrorKind::Api); CHECK(recorder.failed == 1);
                if (mode == 0) {
                    const std::string expected_message = wire == Wire::Responses
                        ? "native source failure (type=native.source.failure, code=native.source.failure)"
                        : "native source failure";
                    CHECK(sampled.error.message == expected_message);
                    CHECK(sampled.error.api_code == "native.source.failure");
                }
                if (mode == 2 || mode == 3 || mode == 5 || mode == 6)
                    CHECK(sampled.error.api_code == "sample.backend_exception");
                if (mode == 2) CHECK(sampled.error.message.find("actual Connection callback fault") != std::string::npos);
                if (mode == 5) CHECK(sampled.error.message.find("actual Connection identity callback fault") != std::string::npos);
                if (mode == 4 || mode == 7) CHECK(sampled.error.api_code == "sample.backend_unknown_exception");
            }
            agent::BackgroundCallAccounting accounting; agent::AddSampleAccounting(&accounting, sampled);
            CHECK(accounting.usage.input_tokens == sampled.usage.input_tokens);
            CHECK(accounting.usage.output_reasoning_tokens == sampled.usage.output_reasoning_tokens);
            CHECK(accounting.usage_coverage.samples == 1);
            backend.connection.reset();
            server.StopAndJoin(); CHECK(server.owned_threads_quiescent());
            REQUIRE(server.requests().size() == 1); CHECK(server.requests()[0].method == "POST");
        }
    Mark(module, "native-connection-faults");
}

inline void LearningTextGate(const Port& port,const char* module) {
    const auto cjk=port.text_stats("一二三四五六七八");
    CHECK(cjk.unicode_scalar_count==8); CHECK(cjk.cjk_char_count==8); CHECK(cjk.latin_word_count==0);
    CHECK(port.minimum_text(cjk)); CHECK_FALSE(port.text_gate(cjk,false).has_value());
    const auto latin=port.text_stats("alpha beta gamma"); CHECK(latin.latin_word_count==3); CHECK(port.minimum_text(latin));
    const auto code=port.text_stats("`foo` `bar` test"); CHECK(code.code_token_count==2); CHECK(port.minimum_text(code));
    const auto ack=port.text_stats("ok"); REQUIRE(port.text_gate(ack,false).has_value());
    CHECK(*port.text_gate(ack,false)==agent::memory_learning_gates::ExtractionSkipReason::AcknowledgementOnly);
    REQUIRE(port.text_gate(ack,true).has_value()); CHECK(*port.text_gate(ack,true)==agent::memory_learning_gates::ExtractionSkipReason::ShortText);
    const auto command=port.text_stats(" /help"); REQUIRE(port.text_gate(command,true).has_value());
    CHECK(*port.text_gate(command,true)==agent::memory_learning_gates::ExtractionSkipReason::SlashCommandOnly);
    const auto mixed=port.text_stats("ok please fix the actual build"); CHECK_FALSE(mixed.only_acknowledgement);
    Mark(module,"learning-text-gate");
}
inline void LearningEvidenceGate(const Port& port,const char* module) {
    const auto has=[](const std::vector<std::string>& values,const char* name) { return std::find(values.begin(),values.end(),name)!=values.end(); };
    const auto preference=port.turn_signals("以后简短","好。",false); CHECK(has(preference,"preference_or_correction"));
    CHECK(port.turn_signals("普通问题","以后简短，记住",true).empty());
    CHECK(port.turn_signals("普通问题","tests passed",false).empty());
    const auto verified=port.turn_signals("普通问题","tests passed",true); CHECK(has(verified,"test_conclusion"));
    CHECK_FALSE(has(verified,"explicit_remember_unsaved")); CHECK_FALSE(has(verified,"preference_or_correction"));
    const auto text=std::string("记住这个项目用 pnpm"); const auto stats=port.text_stats(text);
    CHECK(has(port.durable_signals(text,stats,false,false),"explicit_remember_unsaved"));
    CHECK_FALSE(has(port.durable_signals(text,stats,false,true),"explicit_remember_unsaved"));
    Mark(module,"learning-evidence-gate");
}
inline void LearningReasonNames(const Port& port,const char* module) {
    using Reason=agent::memory_learning_gates::ExtractionSkipReason;
    const std::pair<Reason,const char*> values[]={{Reason::Disabled,"disabled"},{Reason::NoNewHistory,"no_new_history"},
        {Reason::EmptyTranscript,"empty_transcript"},{Reason::PromptMissing,"prompt_missing"},{Reason::AlreadyMutated,"already_mutated"},
        {Reason::ShortText,"short_text"},{Reason::AcknowledgementOnly,"acknowledgement_only"},{Reason::SlashCommandOnly,"slash_command_only"},
        {Reason::ExtractModeOff,"extract_mode_off"},{Reason::NoDurableSignal,"no_durable_signal"}};
    for(const auto& [value,name]:values) CHECK(std::string(port.skip_name(value))==name);
    Mark(module,"learning-reason-names");
}

}  // namespace memory_extraction_fixture
