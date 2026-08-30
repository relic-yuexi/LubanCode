// runtime 层的请求 usage 账(Token 账本单 A0 §6.1.2/§11.2)。
//
// 两种 request id 分家:
//   local_request_id    AgentLoop/ModelRequestRecorder 发请求前铸的本地号,
//                       Journal 里 prepared/sent/output/usage 的关联主键;
//   provider_response_id  provider 在 MessageStart/response.created 里回的
//                       外部号,只作对账,可空。
//
// api::UsageReport 只装 provider 视角(含 provider_response_id);本地号由
// runtime 这只结构包住——api 层不认 runtime 身份,方向不可倒。Trajectory
// 接线(A1)从同一只 record 提交 model.usage.recorded,不许从 UI
// ServerEvent 反推 canonical usage。
#pragma once

#include <string>

#include "api/types.hpp"

namespace lubancode::runtime {

struct RequestUsageRecord {
    std::string local_request_id;
    int attempt = 1;
    api::UsageReport provider_usage;
};

}  // namespace lubancode::runtime
