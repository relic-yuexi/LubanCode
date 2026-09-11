// v3 语义校验(docs/architecture/trajectory-v3-schema.md §二/§三)。
//
// 信封层(envelope.hpp)管"形状":键白名单、类型、枚举。本件管"合同":
//   - kind ↔ status 固定映射(§2.2);
//   - 按 kind 的必选身份字段(compact.* 必带 compactId 等);
//   - 关键 payload 子字段(prepared 的 inputMessageRefs、applied 的
//     contextChain 等);
//   - message 行按 role/purpose 的约束(system turnId=null、assistant 来源
//     三件套与 usage 键、摘要 turnId=null + sourceMessageRef);
//   - 引用格式(同会话 string / 跨会话五键对象)与链节点结构。
//
// 只验单行可判定的合同;跨行语义(引用目标存在、链连通、seq 连续、
// 哈希衔接)在 writer 与校验脚本。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/envelope.hpp"

namespace lubancode::trajectory::v3 {

struct Schema3Error {
    std::string code;
    std::string message;
};

// message 行语义校验;通过返回 nullopt。
std::optional<Schema3Error> ValidateMessageLine(const MessageLine& line);

// event 行语义校验;通过返回 nullopt。
std::optional<Schema3Error> ValidateEventLine(const EventLine& line);

// 引用格式(§3.1):合法 = 非空 string(同会话)或五键对象
// {sessionId, runId, seq, id, hash}(跨会话)。
bool IsValidRef(const nlohmann::json& ref);

// 事件信封携带的引用字段(payload 里按 key 取值)是否合法格式。
std::optional<Schema3Error> CheckRefField(std::string_view context, const nlohmann::json& payload,
                                          const char* key, bool required);

// 链节点数组(§2.4):每项 {messageRef, prevMessageRef};根唯一且前驱
// null、非根前驱存在(在数组内)、无重复、邻接一致、无环、连通。
std::optional<Schema3Error> ValidateContextChain(std::string_view context,
                                                 const std::vector<nlohmann::json>& chain);

// 追加链片段(context.input.applied 的 appendedChain,§2.4):非空、
// 首节点前驱非 null(追加必接旧尾)、无重复、邻接一致、无环连通;
// 首节点前驱可指链外旧尾,不适用"根唯一"。
std::optional<Schema3Error> ValidateAppendedChain(std::string_view context,
                                                  const std::vector<nlohmann::json>& chain);

// 从 payload 取链节点数组并校验(缺键/类型错给稳定码)。
std::optional<Schema3Error> CheckContextChainField(std::string_view context,
                                                   const nlohmann::json& payload,
                                                   const char* key = "contextChain");

// usage 结构(§五):object;已知子键(inputTokens/outputTokens/reasoning
// Tokens/cacheReadTokens/cacheWriteTokens)须为非负整数;缺子项省键。
std::optional<Schema3Error> ValidateUsage(const nlohmann::json& usage);

// artifactRef(§3.1 六键,结果仓):{artifactId,kind,path,sha256,bytes,
// mediaType},kind ∈ result_metadata|stdout|stderr|combined|raw_payload|report|image|blob。
std::optional<Schema3Error> ValidateArtifactRef(std::string_view context,
                                                const nlohmann::json& ref);

}  // namespace lubancode::trajectory::v3
