// 模型请求用途枚举(Token 账本单 §6.2 A0 冻结)。
//
// model.request.prepared.payload.purpose 的合法值封闭在这张表里;调用方
// 不知道用途便不许提交 prepared 事件,不能全塞 main_turn。usage 事件按
// local request_id + attempt 与 prepared 合账,二者缺一样,sample 标
// incomplete_linkage,不可猜。
#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace lubancode::accounting {

enum class RequestPurpose {
    MainTurn,           // 主会话回合
    SubagentTurn,       // 子代理回合
    WorkflowNode,       // 工作流节点
    CompactMap,         // compact 的分块映射请求
    CompactReduce,      // compact 的归并请求
    MemoryExtract,      // 记忆抽取
    TitleRefine,        // 标题打磨
    DoctorProbe,        // /doctor 诊断探针
    GoalContinue,       // 持久目标续跑
    LoopIteration,      // 循环迭代
    InsightsModelReview,  // /insights --model-review 的评议请求
    OtherHostRequest,   // 宿主旁路请求(以上都不是)
};

// 线上名与枚举一一对应;改线上名即改合同。
const char* PurposeName(RequestPurpose purpose);
// 严格解析:认不得的名字给 nullopt,不猜、不落 other。
std::optional<RequestPurpose> PurposeFromName(std::string_view name);
// 全部合法值(枚举序;测试遍历用)。
const std::vector<RequestPurpose>& AllPurposes();

}  // namespace lubancode::accounting
