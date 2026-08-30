// wire fixture 库对账与回放(模型协议兼容实录矩阵单,P0):
//   1) 手册 snapshot hash:三份本地兼容手册的 sha256 与 fixture manifest
//      里记的对账——手册一变,这里红,提醒人核对 fixture 后更新 hash;
//   2) loader 全量校验:manifest 必填齐、id 不重、四家 wire 各有册;
//   3) 逐册回放:按 fixture 的 wire 喂对应 parser(整帧,与迁出前
//      inline 测试同一喂法,行为不改),事件类型序列/usage/stop_reason
//      与 manifest 对账。切法重放(1 字节/随机小块/并帧)是 L3 的账,
//      在 wire_replay 集成册里另算。

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "api/anthropic/events.hpp"
#include "api/chat/events.hpp"
#include "api/gemini/events.hpp"
#include "api/responses/events.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"
#include "api_fixture.hpp"

using namespace lubancode;
using lubancode_test::ApiFixture;

namespace {

// L3 的四种切法(规格"每册 wire 实录按 1 字节、随机小块、整帧、并帧四种
// 切法重放"):随机小块用固定 seed,失败时 CAPTURE 出来,能复跑同一刀口。
constexpr std::uint32_t kChunkSeed = 20260827;

enum class ChunkMode { Whole, OneByte, RandomFixed, PairFrames };

std::vector<std::string> ChunkBytes(const std::string& stream, ChunkMode mode) {
    std::vector<std::string> chunks;
    switch (mode) {
    case ChunkMode::Whole:
        chunks.push_back(stream);
        break;
    case ChunkMode::OneByte:
        for (const char byte : stream) chunks.emplace_back(1, byte);
        break;
    case ChunkMode::RandomFixed: {
        std::mt19937 rng(kChunkSeed);
        std::uniform_int_distribution<int> size(3, 11);
        for (std::size_t i = 0; i < stream.size();) {
            const std::size_t take = std::min<std::size_t>(size(rng), stream.size() - i);
            chunks.push_back(stream.substr(i, take));
            i += take;
        }
        break;
    }
    case ChunkMode::PairFrames: {
        // 按空行切帧,两帧并一块发(并帧)。
        std::size_t begin = 0;
        std::vector<std::string> frames;
        while (begin < stream.size()) {
            const std::size_t sep = stream.find("\n\n", begin);
            const std::size_t end = sep == std::string::npos ? stream.size() : sep + 2;
            frames.push_back(stream.substr(begin, end - begin));
            begin = end;
        }
        for (std::size_t i = 0; i < frames.size(); i += 2) {
            chunks.push_back(i + 1 < frames.size() ? frames[i] + frames[i + 1] : frames[i]);
        }
        break;
    }
    }
    return chunks;
}

api::SseFrame Frame(const std::pair<std::string, std::string>& raw) {
    return api::SseFrame{raw.first.empty() ? std::string("message") : raw.first, raw.second};
}

// 中立事件的类型名(与 manifest expected_events 的记法一致)。
std::string EventName(const api::StreamEvent& event) {
    switch (event.index()) {
    case 0: return "MessageStart";
    case 1: return "TextDelta";
    case 2: return "ThinkingDelta";
    case 3: return "ToolUseStart";
    case 4: return "ToolUseInputDelta";
    case 5: return "ContentBlockDone";
    case 6: return "BuiltinToolStart";
    case 7: return "BuiltinToolDone";
    case 8: return "MessageDone";
    case 9: return "ImageOutput";
    default: return "StreamError";
    }
}

// 一册 fixture 按自家 wire 喂对应 parser,吐中立事件序列。字节先按切法
// 分块,经生产同一只 SseFramer 切帧——单帧能解、跨 chunk 就坏这类病在
// 这层现形。
std::vector<api::StreamEvent> Replay(const ApiFixture& fixture, ChunkMode mode) {
    std::vector<api::StreamEvent> events;
    std::vector<api::SseFrame> frames;
    api::SseFramer framer;
    for (const auto& chunk : ChunkBytes(fixture.stream, mode)) {
        for (auto& frame : framer.feed(chunk)) frames.push_back(std::move(frame));
    }
    REQUIRE_FALSE(framer.overflowed());
    const auto raw_frames = [&] {
        std::vector<std::pair<std::string, std::string>> raw;
        for (const auto& frame : frames) raw.emplace_back(frame.event, frame.data);
        return raw;
    }();
    if (fixture.wire == "anthropic-messages") {
        api::anthropic::EventParser parser(fixture.scenario == "post_tool_raw_think_tags_in_text_delta");
        for (const auto& raw : raw_frames) {
            for (auto& event : parser.Consume(Frame(raw))) {
                events.push_back(std::move(event));
            }
        }
        for (auto& event : parser.Finish()) events.push_back(std::move(event));
    } else if (fixture.wire == "openai-responses") {
        for (const auto& raw : raw_frames) {
            if (auto event = api::responses::parse_event(Frame(raw)); event.has_value()) {
                events.push_back(*event);
            }
        }
    } else if (fixture.wire == "openai-chat-completions") {
        api::chat::EventParser parser;
        for (const auto& raw : raw_frames) {
            for (auto& event : parser.Consume(Frame(raw))) {
                events.push_back(std::move(event));
            }
        }
        for (auto& event : parser.Finish()) {
            events.push_back(std::move(event));
        }
    } else {
        api::gemini::EventParser parser;
        for (const auto& raw : raw_frames) {
            for (auto& event : parser.Consume(Frame(raw))) {
                events.push_back(std::move(event));
            }
        }
        for (auto& event : parser.Finish()) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

const api::MessageDone* FinalDone(const std::vector<api::StreamEvent>& events) {
    for (const auto& event : events) {
        if (const auto* done = std::get_if<api::MessageDone>(&event); done != nullptr) {
            return done;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("fixture 手册 hash 对账:三份手册的 sha256 与 manifest 记录一致") {
    // 手册字节一变(哪怕只加一段无关内容),这里就红:对账的人去核对
    // 引它的 fixture 是否还成立,成立才更新 manifest 里的 hash。
    const std::map<std::string, std::string> hashes = {
        {"OpenAI兼容-Responses.md", "6917b0a1aad63824c585bfe0cb22c392795a3605c992f5886fb120765c37ad54"},
        {"OpenAI兼容-Chat.md", "0a4ae02111e2f6fdca649f49b09ce09ea57e51122b38971cc8ee2df2e068fbab"},
        {"Anthropic兼容-Messages.md", "7e35698415e8c7ff5dac8f3b49faedd1bf659bda7d1abd2fe2f401daf8118fcc"},
    };
    for (const auto& [manual, expected] : hashes) {
        const std::string actual = lubancode_test::Sha256File(lubancode_test::ManualPath(manual));
        INFO("手册: ", manual);
        REQUIRE_FALSE(actual.empty());
        CHECK(actual == expected);
    }
}

TEST_CASE("fixture loader:全库可读、id 不重、四家 wire 各有册、手册 hash 与正文一致") {
    const auto all = lubancode_test::LoadAllApiFixtures();
    REQUIRE(all.has_value());
    REQUIRE(all->size() >= 8);

    std::map<std::string, int> per_wire;
    for (const auto& fixture : *all) {
        per_wire[fixture.wire] += 1;
        // 手册来源:hash 与当前手册一致(internal 来源跳过)。
        if (fixture.source_document != "internal") {
            const std::string actual =
                lubancode_test::Sha256File(lubancode_test::ManualPath(fixture.source_document));
            INFO("fixture: ", fixture.fixture_id, " 手册: ", fixture.source_document);
            CHECK_FALSE(actual.empty());
            CHECK(actual == fixture.doc_snapshot_hash);
        }
        // 事件账至少一桩,usage 期望是 object。
        CHECK_FALSE(fixture.expected_events.empty());
        CHECK(fixture.usage_expectation.is_object());
    }
    CHECK(per_wire["anthropic-messages"] >= 3);
    CHECK(per_wire["openai-chat-completions"] >= 2);
    CHECK(per_wire["openai-responses"] >= 2);
    CHECK(per_wire["google-generate-content"] >= 1);
}

TEST_CASE("fixture 回放:每册事件类型序列/usage/stop_reason 与 manifest 对账(四种切法)") {
    const auto all = lubancode_test::LoadAllApiFixtures();
    REQUIRE(all.has_value());
    // 整帧、1 字节、随机小块(seed 固定)、并帧——四种刀口下的产出必须
    // 逐桩一致:切法只该改"字节怎么到",不许改"解出什么"。
    const std::vector<std::pair<ChunkMode, const char*>> modes = {
        {ChunkMode::Whole, "整帧"}, {ChunkMode::OneByte, "1 字节"},
        {ChunkMode::RandomFixed, "随机小块"}, {ChunkMode::PairFrames, "并帧"}};
    for (const auto& fixture : *all) {
        for (const auto& [mode, mode_name] : modes) {
            // doctest 的 CAPTURE 只收一枚实参——MSVC 老预处理器闷声吞多参,
            // GCC/clang 按规格拒(CI 一日三雷同族);拆成三枚。
            CAPTURE(fixture.fixture_id);
            CAPTURE(mode_name);
            CAPTURE(kChunkSeed);
            const auto events = Replay(fixture, mode);

        std::vector<std::string> names;
        names.reserve(events.size());
        for (const auto& event : events) names.push_back(EventName(event));
        REQUIRE(names.size() == fixture.expected_events.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            INFO("fixture ", fixture.fixture_id, " 第 ", i, " 桩事件: got=", names[i],
                 " want=", fixture.expected_events[i]);
            CHECK(names[i] == fixture.expected_events[i]);
        }

        const auto* done = FinalDone(events);
        if (std::find(fixture.expected_events.begin(), fixture.expected_events.end(), "MessageDone") !=
            fixture.expected_events.end()) {
            REQUIRE(done != nullptr);
            if (!fixture.stop_reason.empty()) {
                CHECK(done->stop_reason == fixture.stop_reason);
            }
            const auto expect = [&](const char* field, std::int64_t value) {
                if (fixture.usage_expectation.contains(field)) {
                    CHECK(value == fixture.usage_expectation.at(field).get<std::int64_t>());
                }
            };
            expect("input_tokens", done->usage.input_tokens);
            expect("output_tokens", done->usage.output_tokens);
            expect("cache_read_tokens", done->usage.cache_read_tokens);
            expect("cache_creation_tokens", done->usage.cache_creation_tokens);
            expect("output_reasoning_tokens", done->usage.output_reasoning_tokens);
        } else {
            CHECK(done == nullptr);
        }
        }
    }
}

TEST_CASE("vLLM MiniCPM5 工具后续实录:原始 think 段隔离,不漏进正文与思考历史") {
    const auto fixture =
        lubancode_test::LoadApiFixture("anthropic_messages", "live_vllm_minicpm5_post_tool_raw_think");
    REQUIRE(fixture.has_value());

    const auto events = Replay(*fixture, ChunkMode::Whole);
    std::string text;
    std::string thinking;
    for (const auto& event : events) {
        if (const auto* delta = std::get_if<api::TextDelta>(&event); delta != nullptr) {
            text += delta->text;
        }
        if (const auto* delta = std::get_if<api::ThinkingDelta>(&event); delta != nullptr) {
            thinking += delta->text;
        }
    }

    CHECK(thinking.empty());
    CHECK(text.find("<think>") == std::string::npos);
    CHECK(text.find("</think>") == std::string::npos);
    CHECK(text.find("project(lubancode VERSION") != std::string::npos);
}
