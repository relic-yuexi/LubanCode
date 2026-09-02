// ApprovalClass 生产工具分类完整性：需确认工具不得漏标，None 只配无需确认。
#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/run_command.hpp"
#include "tools/send_session_message_tool.hpp"
#include "tools/tool.hpp"
#include "tools/undo_file_edit.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

TEST_CASE("ApprovalClass: 内置需确认工具分类稳定且 None 只配无需确认") {
    tools::UndoTokenLookup undo_lookup;
    std::vector<std::unique_ptr<tools::Tool>> tools_under_test;
    tools_under_test.push_back(std::make_unique<tools::WriteFileTool>());
    tools_under_test.push_back(std::make_unique<tools::EditFileTool>());
    tools_under_test.push_back(std::make_unique<tools::UndoFileEditTool>(std::move(undo_lookup)));
    tools_under_test.push_back(std::make_unique<tools::RunCommandTool>());
    tools_under_test.push_back(std::make_unique<tools::BackgroundOutputTool>());
    tools_under_test.push_back(std::make_unique<tools::StopBackgroundTool>());

    const std::vector<tools::ApprovalClass> expected{
        tools::ApprovalClass::FileEdit,
        tools::ApprovalClass::FileEdit,
        tools::ApprovalClass::FileDestructive,
        tools::ApprovalClass::Command,
        tools::ApprovalClass::None,
        tools::ApprovalClass::External,
    };
    REQUIRE(tools_under_test.size() == expected.size());
    for (std::size_t i = 0; i < tools_under_test.size(); ++i) {
        CAPTURE(tools_under_test[i]->name());
        CHECK(tools_under_test[i]->approval_class() == expected[i]);
        CHECK((tools_under_test[i]->approval_class() == tools::ApprovalClass::None) ==
              !tools_under_test[i]->needs_confirm());
    }
}

TEST_CASE("ApprovalClass: 跨会话写工具属于 External") {
    tools::SendSessionMessageTool tool([] { return std::vector<peers::PeerCard>{}; },
                                       [](const peers::PeerCard&, const std::string&) {
                                           return peers::PeerDelivery{};
                                       });
    CHECK(tool.needs_confirm());
    CHECK(tool.approval_class() == tools::ApprovalClass::External);
}
