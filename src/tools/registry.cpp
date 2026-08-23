#include "tools/registry.hpp"

#include <utility>

namespace lubancode::tools {

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    ToolRegistration registration;
    registration.tool = std::move(tool);
    Register(std::move(registration));
}

void ToolRegistry::Register(ToolRegistration registration) {
    if (registration.tool == nullptr) {
        return;
    }
    // effect_class/recovery 等声明与 Tool 自身的 override 各说各话时,
    // 注册处那份是"装配层的保守补注",工具自身的 getter 仍归它自己——
    // 诊断层先取注册元数据,缺省再回落工具 getter(见 tool_trace 装配)。
    // 未在注册处声明(仍是保守默认)而工具自己报了更具体的档,采信工具
    // 自己的——声明不能放宽安全,收紧没人会抱怨。
    if (registration.effect_class == EffectClass::InProcessUnknown &&
        registration.recovery == RecoveryCapability::None && registration.idempotency == Idempotency::Unknown) {
        registration.effect_class = registration.tool->effect_class();
        registration.idempotency = registration.tool->idempotency();
        registration.recovery = registration.tool->recovery_capability();
    }
    if (registration.version_or_digest.empty()) {
        registration.version_or_digest = registration.tool->version_or_digest();
    }
    registrations_.push_back(std::move(registration));
    tools_.push_back(std::move(registrations_.back().tool));
    // 名字缓存:tool 已 move 进 tools_,RegistrationOf 按缓存查,不必回头
    // 解引用(也免得未来谁改了所有权顺序又踩一遍)。
    names_.push_back(tools_.back()->name());
}

Tool* ToolRegistry::Find(const std::string& name) const {
    for (const auto& tool : tools_) {
        if (tool->name() == name) {
            return tool.get();
        }
    }
    return nullptr;
}

const ToolRegistration* ToolRegistry::RegistrationOf(const std::string& name) const {
    for (std::size_t i = 0; i < registrations_.size() && i < names_.size(); ++i) {
        if (names_[i] == name) {
            return &registrations_[i];
        }
    }
    return nullptr;
}

}  // namespace lubancode::tools
