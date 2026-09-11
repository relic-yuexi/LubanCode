#include <doctest/doctest.h>
#include "api/types.hpp"
#include "api/chat/request.hpp"

using namespace lubancode;

TEST_CASE("Sanitizing committed tool previews never reconstructs text from rich payloads") {
    api::ToolResultBlock result;
    result.tool_use_id = "call-1";
    result.content = "selected preview\n";
    result.preview_committed = true;
    result.blocks.push_back(tools::TextContent{result.content});
    tools::ResourceLinkContent source;
    source.uri = "https://example.com/" + std::string(40000, 'x');
    result.blocks.push_back(std::move(source));
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(result);
    api::SanitizeMessage(message);
    const auto& adopted = std::get<api::ToolResultBlock>(message.content[0]);
    CHECK(adopted.content == result.content);
    CHECK(adopted.blocks.size() == 2);
    api::Request request;
    request.model = "test";
    request.messages.push_back(message);
    const auto wire = api::chat::BuildRequestJson(request);
    CHECK(wire.at("messages")[0].at("content") == result.content);
}

TEST_CASE("Uncommitted tool payloads still refresh their text projection") {
    api::ToolResultBlock result;
    result.content = "old projection";
    result.blocks.push_back(tools::TextContent{"new projection"});
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(result);
    api::SanitizeMessage(message);
    CHECK(std::get<api::ToolResultBlock>(message.content[0]).content == "new projection");
}
