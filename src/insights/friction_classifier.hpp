// FrictionClassifier(Token 账本单 §9.3/A4)。
//
// 输入一条 stream 已经验过的轨迹事件(按 seq 升序),输出摩擦发生
// (occurrence):类名 + 事件引用。允许一件事挂多类(§9.3"不能为凑百分比
// 强行单选")。
//
// v1 只认 Journal 里有确凿证据的类:
//   tool.execution_failure / tool.invalid_input / tool.repeated_retry /
//   tool 参数重试、permission.denied、verification.failure /
//   verification.missing、provider.failure、cancelled、approval.wait。
// 其余类(request.ambiguity、instruction.conflict、context.loss、
// context.churn、user.correction、budget.limit)需要对话语义或宿主事件
// 之外的信号,单凭 Journal 断不了——不硬猜,枚举仍在册
// (AllFrictionCategories 返回 §9.3 全表),分析器没证据就不出。
#pragma once

#include <string>
#include <vector>

#include "insights/finding.hpp"
#include "trajectory/event.hpp"

namespace lubancode::insights {

inline constexpr const char* kFrictionRuleVersion = "friction-v1";

// 一次摩擦:类名(§9.3 枚举)+ 证据(事件引用)。
struct FrictionOccurrence {
    std::string category;
    EvidenceItem evidence;

    // rule_version 按类带账(analyzer 版本账的组成部分)。
    std::string rule_version;  // "friction-v1:<规则号>"
};

// §9.3 的封闭类名表(排序稳定;在册不等于本版能判)。
const std::vector<std::string>& AllFrictionCategories();

// 分类(纯函数)。事件按 seq 升序喂;返回按出现序稳定。
std::vector<FrictionOccurrence> ClassifyFriction(
    const std::vector<trajectory::EventEnvelope>& events);

}  // namespace lubancode::insights
