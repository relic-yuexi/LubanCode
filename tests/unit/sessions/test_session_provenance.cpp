// session JSONL provenance 单测(多渠道消息接入单阶段 3):消息行带宿主
// 真账、向后兼容(老版本读新档不坏、新版本读老档回落推断)。
//
// 真源:docs/architecture/channels/message-contracts.md §2。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "channel/types.hpp"
#include "sessions/session_store.hpp"

using namespace lubancode;

namespace {

api::Message UserText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

channel::MessageProvenance ChannelProvenance() {
    channel::MessageProvenance provenance;
    provenance.origin = channel::MessageOrigin::ExternalChannel;
    provenance.channel_id = "qqbot";
    provenance.account_id = "main";
    provenance.sender_id = "owner-openid";
    provenance.conversation_id = "dm-1";
    provenance.provider_message_id = "m-42";
    return provenance;
}

}  // namespace

TEST_CASE("带 provenance 的消息行:往返无损") {
    const std::string line =
        sessions::SerializeSessionMessageWithProvenance(UserText("帮我看看"), ChannelProvenance(),
                                                        "2026-09-01 10:00:00");
    const auto record = sessions::ParseSessionMessageWithProvenance(line);
    REQUIRE(record.has_value());
    CHECK(record->message.role == api::Role::User);
    REQUIRE(record->message.content.size() == 1);
    const auto* text = std::get_if<api::TextBlock>(&record->message.content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text == "帮我看看");
    REQUIRE(record->provenance.has_value());
    CHECK(record->provenance->origin == channel::MessageOrigin::ExternalChannel);
    CHECK(record->provenance->channel_id == "qqbot");
    CHECK(record->provenance->account_id == "main");
    CHECK(record->provenance->sender_id == "owner-openid");
    CHECK(record->provenance->conversation_id == "dm-1");
    CHECK(record->provenance->provider_message_id == "m-42");
}

TEST_CASE("向后兼容:老消息行(无 provenance)解析得 nullopt 账") {
    const std::string legacy =
        sessions::SerializeSessionMessage(UserText("老档消息"), "2026-08-01 10:00:00");
    const auto record = sessions::ParseSessionMessageWithProvenance(legacy);
    REQUIRE(record.has_value());
    CHECK(record->message.role == api::Role::User);
    CHECK_FALSE(record->provenance.has_value());
}

TEST_CASE("向后兼容:老解析器读新档不坏(未知键忽略,消息账无损)") {
    const std::string line =
        sessions::SerializeSessionMessageWithProvenance(UserText("新档消息"), ChannelProvenance(),
                                                        "2026-09-01 10:00:00");
    // 老路:DeserializeSessionMessage 只取 role/content,provenance 键被忽略。
    const auto message = sessions::DeserializeSessionMessage(line);
    REQUIRE(message.has_value());
    CHECK(message->role == api::Role::User);
    REQUIRE(message->content.size() == 1);
}

TEST_CASE("老档 provenance 兼容推断:user 回 HumanTerminal,assistant 回 HostSynthetic") {
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"答"});

    const auto user_origin = sessions::InferLegacyProvenance(UserText("问"));
    CHECK(user_origin.origin == channel::MessageOrigin::HumanTerminal);
    CHECK(user_origin.channel_id.empty());  // 猜不出渠道就留白,不靠文字标签猜

    const auto assistant_origin = sessions::InferLegacyProvenance(assistant);
    CHECK(assistant_origin.origin == channel::MessageOrigin::HostSynthetic);
}

TEST_CASE("坏 provenance 行不废消息:回落 nullopt 交推断") {
    nlohmann::json j = nlohmann::json::object();
    j["role"] = "user";
    j["content"] = nlohmann::json::array();
    nlohmann::json bad_provenance = nlohmann::json::object();
    bad_provenance["origin"] = "galaxy_far_away";  // 认不得的 origin
    j["provenance"] = bad_provenance;
    const auto record = sessions::ParseSessionMessageWithProvenance(j.dump());
    REQUIRE(record.has_value());
    CHECK(record->message.role == api::Role::User);
    CHECK_FALSE(record->provenance.has_value());
}

TEST_CASE("SessionStore:AppendMessageWithProvenance 落盘且能读回") {
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("lubancode_prov_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    sessions::SessionStore store(dir.string());
    REQUIRE(store.Begin(sessions::SessionMeta{}, "prov-session"));
    CHECK(store.AppendMessageWithProvenance(UserText("渠道来信"), ChannelProvenance()));
    CHECK(store.AppendMessage(UserText("终端补一句")));

    std::ifstream in(store.file_path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 首行 meta,次行渠道消息带 provenance,三行终端消息不带。
    const auto first_newline = content.find('\n');
    const auto second_newline = content.find('\n', first_newline + 1);
    const std::string channel_line =
        content.substr(first_newline + 1, second_newline - first_newline - 1);
    const std::string terminal_line = content.substr(second_newline + 1);
    const auto channel_record = sessions::ParseSessionMessageWithProvenance(channel_line);
    REQUIRE(channel_record.has_value());
    REQUIRE(channel_record->provenance.has_value());
    CHECK(channel_record->provenance->sender_id == "owner-openid");
    CHECK(sessions::ParseSessionMessageWithProvenance(terminal_line)->provenance ==
          std::nullopt);

    std::error_code cleanup;
    std::filesystem::remove_all(dir, cleanup);
}
