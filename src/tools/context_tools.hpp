// context_search / context_read:渐进式上下文仓的两把只读钥匙(第二期)。
//
// 模型在请求视图里看见 [artifact a0007 ...] 时,凭 artifact_id 用这两把
// 钥匙追回证据:先搜(命中行 + 块 + 小片预览),再按块或行窗读。硬规矩:
//   - 只读,不落盘、不改仓;
//   - scope 只认稳定 artifact_id——store 由会话注入(当前会话 + 同仓共享
//     的子代理任务),工具不吃磁盘路径,递不进任意文件;
//   - read 守字节预算,超了拒绝并给可用范围,不悄悄截;
//   - blob hash 不合立即隔离(不在工具层兜,是仓的 ReadBlobVerified 门)。
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/artifact_store.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 会话层持有一只共享仓(shared_ptr:会话建档时才 Open,工具构造时仓还没
// 开——工具持同一块内存,Open 之后自然可用;没开的仓一切操作安全退化)。
class ContextSearchTool final : public Tool {
public:
    explicit ContextSearchTool(std::shared_ptr<lubancode::agent::ContextArtifactStore> store);

    std::string name() const override { return "context_search"; }
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    std::shared_ptr<lubancode::agent::ContextArtifactStore> store_;
};

class ContextReadTool final : public Tool {
public:
    using SummarizeArtifact = std::function<std::expected<std::string, std::string>(
        const lubancode::agent::ArtifactRef&)>;

    explicit ContextReadTool(std::shared_ptr<lubancode::agent::ContextArtifactStore> store,
                             SummarizeArtifact summarize = {});

    std::string name() const override { return "context_read"; }
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    std::shared_ptr<lubancode::agent::ContextArtifactStore> store_;
    SummarizeArtifact summarize_;
};

}  // namespace lubancode::tools
