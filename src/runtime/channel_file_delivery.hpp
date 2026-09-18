#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include "gateway/reply_outbox.hpp"
#include "tools/registry.hpp"

namespace lubancode::runtime {

// 每轮只投递一件文件。工具先冻结原件，回复入箱时附上；失败轮不偷发。
// 工作键派生的清单让 selection 已提交后的恢复沿用同一原件。
class ChannelFileDeliveryScope {
public:
    ChannelFileDeliveryScope(std::filesystem::path workspace, std::filesystem::path staging,
                             std::string operation_id);
    ~ChannelFileDeliveryScope();
    ChannelFileDeliveryScope(const ChannelFileDeliveryScope&) = delete;
    ChannelFileDeliveryScope& operator=(const ChannelFileDeliveryScope&) = delete;
    static tools::Tool::Result Stage(const nlohmann::json& input);
private:
    std::filesystem::path workspace_, staging_;
    std::string operation_id_;
    ChannelFileDeliveryScope* previous_ = nullptr;
    static thread_local ChannelFileDeliveryScope* current_;
};

void RegisterChannelFileTool(tools::ToolRegistry& registry);
std::optional<gateway::DurableReplyOutbox::ChannelAttachment> StagedChannelFile(
    const std::filesystem::path& staging, const std::string& operation_id);

}  // namespace lubancode::runtime
