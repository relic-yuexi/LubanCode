// 报告字段 allowlist(Token 账本单 §13/§15.6 A0 冻结)。
//
// 脱敏顺序(§13.2):canonical recorder pre-write redaction -> analyzer
// defensive redaction -> report field allowlist -> HTML escape -> 可选
// model-review allowlist。本件落第三层:report JSON 的合法字段路径树。
// schema 之外多出的字段一律不许过——防哪条旁路把正文带进报告。
//
// FD-06:secret 扫描/替换已下沉 privacy/secret_scan(中立件,
// insights/telemetry/trajectory 三家共用),本件不再声明扫描符号。
#pragma once

#include <string>
#include <vector>

namespace lubancode::insights {

// 报告字段 allowlist(§13.2 第三层):report JSON 的合法字段路径树。
// path 是逐级键名(数组下标不进 path,数组元素按同名字段裁)。报告
// schema 之外多出的字段一律不许过——防哪条旁路把正文带进报告。
bool IsAllowedReportFieldPath(const std::vector<std::string>& path);

}  // namespace lubancode::insights
