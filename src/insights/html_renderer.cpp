// HTML renderer 的实现。纯拼装:escape -> 属性 -> 结构,三步走;动态
// 字符串绝不进 JS 文本(JS 只读 DOM 属性),CSP 哈希对内联块逐字节算。
#include "insights/html_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "hooks/hash.hpp"    // Sha256Hex(CSP 哈希的同一把尺)
#include "insights/redaction.hpp"  // 防御性脱敏(§13.2 第二层,先于 escape)

namespace lubancode::insights {
namespace {

// ---- 基础件 ----

// 动态文本的统一口:先防御性脱敏(§13.2——secret 整段替成
// [REDACTED:<kind>],不保留前后几位),再 HTML escape,最后剥控制字符。
// Journal 侧已有 recorder 写前脱敏,这里是渲染层的第二道闸。
std::string EscapeHtml(const std::string& raw) {
    const std::string text = lubancode::insights::RedactSecrets(raw);
    std::string out;
    out.reserve(text.size() + 16);
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
                    out += ' ';  // 控制字符不进 DOM(坏 UTF-8 的防御位)
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// token 数的紧凑写法(1234 -> "1.2k";不引 cli,领域层自己折)。
std::string FormatTokens(std::int64_t value) {
    char buffer[32];
    if (value < 0) {
        return "0";
    }
    const double magnitude = static_cast<double>(value);
    if (magnitude >= 999'950'000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1fB", std::round(magnitude / 1e8) / 10.0);
    } else if (magnitude >= 999'950.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM", std::round(magnitude / 1e5) / 10.0);
    } else if (magnitude >= 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1fk", std::round(magnitude / 100.0) / 10.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    }
    return buffer;
}

std::string FormatNumber(std::int64_t value) {
    return std::to_string(value);
}

// hex(64 位) -> base64(32 字节摘要 -> 44 字符,CSP hash-source 要的形态)。
std::string HexDigestToBase64(const std::string& hex) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::string();
        }
        bytes += static_cast<char>((hi << 4) | lo);
    }
    std::string base64;
    base64.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0u;
        const unsigned b2 = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0u;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        base64 += kAlphabet[(triple >> 18) & 0x3f];
        base64 += kAlphabet[(triple >> 12) & 0x3f];
        base64 += i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3f] : '=';
        base64 += i + 2 < bytes.size() ? kAlphabet[triple & 0x3f] : '=';
    }
    return base64;
}

// 内联块(样式/脚本)的原文——字节稳定,CSP 哈希逐字节对着它算。
const char* const kStyleSheet =
    "body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;margin:0;background:#f6f7f9;"
    "color:#1c1e21;line-height:1.55}\n"
    "main{max-width:60rem;margin:0 auto;padding:1rem 1.25rem 3rem}\n"
    "h1{font-size:1.35rem;margin:.4rem 0 .2rem}\n"
    "h2{font-size:1.08rem;margin:1.8rem 0 .5rem;border-bottom:1px solid #d8dce1;padding-bottom:.25rem}\n"
    "p{margin:.35rem 0}\n"
    "table{border-collapse:collapse;width:100%;margin:.5rem 0;font-size:.88rem;background:#fff}\n"
    "th,td{border:1px solid #d8dce1;padding:.3rem .5rem;text-align:left;vertical-align:top}\n"
    "th{background:#eef1f4}\n"
    "code{background:#eef1f4;padding:0 .25rem;border-radius:3px;font-size:.85em}\n"
    ".facts td:first-child{width:7.5em;color:#5a6068}\n"
    ".muted{color:#5a6068}\n"
    ".badge{display:inline-block;border:1px solid #c3c9cf;border-radius:9px;padding:0 .45em;"
    "font-size:.78rem;margin:0 .15rem .15rem 0;background:#fff}\n"
    ".filters{display:flex;flex-wrap:wrap;gap:.5rem;margin:.6rem 0;font-size:.85rem}\n"
    ".filters label{color:#5a6068}\n"
    "footer{margin-top:2.5rem;color:#5a6068;font-size:.8rem;border-top:1px solid #d8dce1;"
    "padding-top:.6rem}\n"
    "ul{margin:.3rem 0;padding-left:1.2rem}\n";

const char* const kScript =
    "(function(){\n"
    "  var ws=document.getElementById('f-workspace');\n"
    "  var st=document.getElementById('f-status');\n"
    "  var oc=document.getElementById('f-outcome');\n"
    "  var ct=document.getElementById('f-category');\n"
    "  var tx=document.getElementById('f-text');\n"
    "  function apply(){\n"
    "    var w=ws.value,s=st.value,o=oc.value,c=ct.value,t=tx.value.toLowerCase();\n"
    "    var rows=document.querySelectorAll('#session-rows tr');\n"
    "    var visible=0;\n"
    "    for(var i=0;i<rows.length;i++){\n"
    "      var r=rows[i];\n"
    "      var cats=r.getAttribute('data-cats');\n"
    "      var ok=(w===''||r.getAttribute('data-workspace')===w)&&\n"
    "        (s===''||r.getAttribute('data-status')===s)&&\n"
    "        (o===''||r.getAttribute('data-outcome')===o)&&\n"
    "        (c===''||(cats&&cats.split(' ').indexOf(c)>=0))&&\n"
    "        (t===''||r.getAttribute('data-session').indexOf(t)>=0);\n"
    "      r.style.display=ok?'':'none';\n"
    "      if(ok)visible++;\n"
    "    }\n"
    "    var counter=document.getElementById('session-visible');\n"
    "    if(counter)counter.textContent=String(visible);\n"
    "  }\n"
    "  [ws,st,oc,ct].forEach(function(e){e.addEventListener('change',apply);});\n"
    "  tx.addEventListener('input',apply);\n"
    "  apply();\n"
    "})();\n";

std::string Sha256Base64(const char* text) {
    return HexDigestToBase64(lubancode::hooks::Sha256Hex(text));
}

std::string SeverityLabel(FindingSeverity severity) {
    switch (severity) {
        case FindingSeverity::Info: return "info";
        case FindingSeverity::Warning: return "warning";
        case FindingSeverity::High: return "high";
    }
    return "?";
}

std::string ConfidenceLabel(FindingConfidence confidence) {
    switch (confidence) {
        case FindingConfidence::Low: return "low";
        case FindingConfidence::Medium: return "medium";
        case FindingConfidence::High: return "high";
    }
    return "?";
}

// 样本场的小徽章串(最多 3 个,超了打 +N)。
std::string SampleBadges(const std::vector<std::string>& ids) {
    std::string out;
    std::size_t shown = 0;
    for (const auto& id : ids) {
        if (shown >= 3) {
            break;
        }
        out += "<span class=\"badge\">" + EscapeHtml(id) + "</span>";
        shown += 1;
    }
    if (ids.size() > shown) {
        out += "<span class=\"badge muted\">+" + std::to_string(ids.size() - shown) + "</span>";
    }
    return out;
}

}  // namespace

std::string RenderInsightsHtml(const InsightsReport& report,
                               const WorkspaceAggregate& agg,
                               const InsightsRenderExtras& extras) {
    std::ostringstream page;
    const std::string css_hash = Sha256Base64(kStyleSheet);
    const std::string js_hash = Sha256Base64(kScript);

    page << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
         << "<meta charset=\"utf-8\">\n"
         << "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
            "style-src 'sha256-" << css_hash << "'; script-src 'sha256-" << js_hash << "'\">\n"
         << "<title>LubanCode 使用洞察报告</title>\n"
         << "<style>" << kStyleSheet << "</style>\n"
         << "</head>\n<body>\n<main>\n";

    // ---- 页眉:事实边界(§9.5 首屏) ----
    page << "<h1>LubanCode 使用洞察报告</h1>\n";
    {
        std::string scope_text;
        if (report.scope.all_workspaces) {
            scope_text = "全部工作区(" + FormatNumber(static_cast<std::int64_t>(
                                                extras.workspace_names.size())) +
                         " 个)";
        } else {
            const std::string key = report.scope.workspace_key;
            const auto name = extras.workspace_names.find(key);
            scope_text = name != extras.workspace_names.end() && !name->second.empty()
                             ? name->second + "(" + key + ")"
                             : key;
        }
        page << "<table class=\"facts\">\n"
             << "<tr><td>范围</td><td>" << EscapeHtml(scope_text) << " · " << EscapeHtml(report.scope.since)
             << " 至 " << EscapeHtml(report.scope.until) << "</td></tr>\n"
             << "<tr><td>样本</td><td>" << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_found))
             << " sessions found · " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_verified))
             << " verified · " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_analyzed))
             << " analyzed";
        if (report.coverage.sessions_pending > 0) {
            page << " · " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_pending))
                 << " pending(未轮上的待分析场,再跑一回继续收)";
        }
        page << "</td></tr>\n"
             << "<tr><td>Usage</td><td>" << FormatNumber(agg.requests_with_usage) << "/"
             << FormatNumber(agg.requests_total) << " requests reported"
             << (agg.requests_unknown > 0
                     ? " · " + FormatNumber(agg.requests_unknown) + " unknown(未报,不折 0)"
                     : std::string())
             << "</td></tr>\n"
             << "<tr><td>Outcome</td><td>" << FormatNumber(agg.sessions_outcome_assessed)
             << " 场有 outcome.assessed(样本 " << FormatNumber(agg.sample_sessions) << " 场)"
             << "</td></tr>\n"
             << "<tr><td>分析方式</td><td>" << EscapeHtml(report.analysis_mode)
             << " · model review off(--model-review 属后续批次 A6)</td></tr>\n"
             << "<tr><td>版本</td><td>analyzer " << EscapeHtml(kInsightsAnalyzerVersion)
             << " · schema " << EscapeHtml(kInsightsReportSchema) << " v"
             << kInsightsReportSchemaVersion << "</td></tr>\n"
             << "<tr><td>生成时刻</td><td>" << EscapeHtml(report.generated_at) << "</td></tr>\n"
             << "</table>\n";
    }

    // ---- 一 工作概览 ----
    page << "<h2 id=\"sec-overview\">一 工作概览</h2>\n";
    {
        std::string outcomes;
        if (agg.outcome_counts.empty()) {
            outcomes = "<span class=\"muted\">无 outcome 评估</span>";
        } else {
            for (const auto& entry : agg.outcome_counts) {
                outcomes += "<span class=\"badge\">" + EscapeHtml(entry.outcome) + " ×" +
                            FormatNumber(entry.sessions) + "</span>";
            }
        }
        page << "<table>\n"
             << "<tr><th>sessions</th><th>turns</th><th>工具调用</th><th>文件(场内去重求和)</th>"
             << "<th>验证</th><th>outcome 分布</th></tr>\n"
             << "<tr><td>" << FormatNumber(agg.sessions) << "(micro " << FormatNumber(agg.micro_sessions)
             << ",usage 照收)</td><td>" << FormatNumber(agg.turns) << "</td><td>"
             << FormatNumber(agg.tool_calls) << "</td><td>" << FormatNumber(agg.files_touched_sum)
             << "</td><td>" << FormatNumber(agg.verifications) << "</td><td>" << outcomes
             << "</td></tr>\n</table>\n";
        if (agg.provisional_sessions > 0) {
            page << "<p class=\"muted\">" << FormatNumber(agg.provisional_sessions)
                 << " 场未封口(include-active 的高水位账,成色 provisional)。</p>\n";
        }
    }

    // ---- 二 Token 与 Cache ----
    page << "<h2 id=\"sec-usage\">二 Token 与 Cache</h2>\n";
    {
        page << "<table>\n"
             << "<tr><th>输入</th><th>cache 读</th><th>cache 写</th><th>输出</th>"
             << "<th>推理(已含在输出)</th><th>费用</th></tr>\n"
             << "<tr><td>" << FormatTokens(agg.input_tokens) << "</td><td>"
             << FormatTokens(agg.cache_read_tokens)
             << (agg.cache_read_ratio_percent.has_value()
                     ? "(" + std::to_string(*agg.cache_read_ratio_percent) + "%,分母=实测输入合计)"
                     : "(比例 unknown:实测输入为 0)")
             << "</td><td>" << FormatTokens(agg.cache_creation_tokens) << "</td><td>"
             << FormatTokens(agg.output_tokens) << "</td><td>" << FormatTokens(agg.reasoning_tokens)
             << "</td><td>not_priced(" << EscapeHtml(extras.pricing_note.empty()
                                                         ? std::string("未配价格表")
                                                         : extras.pricing_note)
             << ";逐笔费用看 /usage)</td></tr>\n</table>\n"
             << "<p class=\"muted\">模型/用途/主子执行/重试/compact 的逐笔分账在 Journal 里,"
                "摘要 schema 未带,本表不猜;逐笔账看 /usage --by。</p>\n";
    }

    // ---- 三 Prompt 构成 ----
    page << "<h2 id=\"sec-prompt\">三 Prompt 构成(runtime 层信号汇总)</h2>\n";
    if (agg.prompt_rollups.empty()) {
        page << "<p>规则没命中不硬凑(";
        if (agg.requests_total == 0) {
            page << "样本内没有模型请求账";
        } else {
            page << "runtime 规则零命中";
        }
        page << ")。</p>\n";
    } else {
        page << "<table>\n<tr><th>规则</th><th>类别</th><th>严重度/置信</th><th>命中</th>"
             << "<th>说明</th><th>建议</th><th>样本场</th></tr>\n";
        for (const auto& rollup : agg.prompt_rollups) {
            std::int64_t affected = 0;
            for (const auto& item : rollup.evidence) {
                if (item.metric == "sessions_affected" && item.value.is_number_integer()) {
                    affected = item.value.get<std::int64_t>();
                }
            }
            page << "<tr><td><code>" << EscapeHtml(rollup.finding_id) << "</code></td><td>"
                 << EscapeHtml(rollup.category) << "</td><td>" << SeverityLabel(rollup.severity)
                 << " / " << ConfidenceLabel(rollup.confidence) << "</td><td>"
                 << FormatNumber(affected) << " 场</td><td>" << EscapeHtml(rollup.summary)
                 << "</td><td>" << EscapeHtml(rollup.recommendation) << "</td><td>"
                 << SampleBadges([&] {
                        std::vector<std::string> ids;
                        for (const auto& item : rollup.evidence) {
                            if (item.metric == "sample_sessions" && item.value.is_array()) {
                                for (const auto& id : item.value) {
                                    if (id.is_string()) {
                                        ids.push_back(id.get<std::string>());
                                    }
                                }
                            }
                        }
                        return ids;
                    }())
                 << "</td></tr>\n";
        }
        page << "</table>\n";
    }

    // ---- 四 摩擦点 ----
    page << "<h2 id=\"sec-friction\">四 摩擦点</h2>\n";
    if (agg.frictions.empty()) {
        page << "<p>样本内零摩擦记录(按场次计;micro 场不作样本)。</p>\n";
    } else {
        page << "<table>\n<tr><th>类别</th><th>场次</th><th>样本场</th></tr>\n";
        for (const auto& rollup : agg.frictions) {
            page << "<tr><td>" << EscapeHtml(rollup.category) << "</td><td>"
                 << FormatNumber(rollup.sessions) << "</td><td>"
                 << SampleBadges(rollup.sample_session_ids) << "</td></tr>\n";
        }
        page << "</table>\n<p class=\"muted\">按场次计(摘要只存类名);事件级次数与引用看 "
                "/prompt audit outcome。</p>\n";
    }

    // ---- 五 交互形状 ----
    page << "<h2 id=\"sec-shape\">五 交互形状</h2>\n";
    {
        page << "<ul>\n"
             << "<li>样本 " << FormatNumber(agg.sample_sessions) << " 场(micro "
             << FormatNumber(agg.micro_sessions) << " 场只进 usage 账,不作交互样本)。</li>\n"
             << "<li>有验证记录的场次:" << FormatNumber(agg.sessions_with_verification)
             << ";outcome 评估过的场次:" << FormatNumber(agg.sessions_outcome_assessed) << "。</li>\n"
             << "<li>验证失败出现 " << FormatNumber(agg.sessions_with_verification_failure)
             << " 场;取消 " << FormatNumber(agg.sessions_cancelled) << " 场;同工具反复重试 "
             << FormatNumber(agg.sessions_repeated_retry) << " 场。</li>\n"
             << "<li>语义类信号(反复澄清/用户纠正/上下文遗失)单凭 Journal 断不了,本报告不猜"
                "(§8.3 边界)。</li>\n"
             << "<li>只摆习惯账,不做人身评价。</li>\n"
             << "</ul>\n";
    }

    // ---- 六 建议 ----
    page << "<h2 id=\"sec-suggestions\">六 建议</h2>\n";
    if (agg.signals.empty()) {
        page << "<p>没有够得上门槛的功能信号(先决不满足就不出,不硬凑)。</p>\n";
    } else {
        page << "<table>\n<tr><th>信号</th><th>现成能力</th><th>动作</th><th>先决条件</th>"
             << "<th>命中</th><th>样本场</th></tr>\n";
        for (const auto& rollup : agg.signals) {
            page << "<tr><td><code>" << EscapeHtml(rollup.signal_id) << "</code></td><td>"
                 << EscapeHtml(rollup.feature) << "</td><td>"
                 << EscapeHtml(rollup.action.empty() ? std::string("看对应场次的信号明细")
                                                     : rollup.action)
                 << "</td><td>" << EscapeHtml(rollup.precondition) << "</td><td>"
                 << FormatNumber(rollup.sessions) << " 场</td><td>"
                 << SampleBadges(rollup.sample_session_ids) << "</td></tr>\n";
        }
        page << "</table>\n<p class=\"muted\">只推荐本仓库现成功能;对应事实账见三/四节,"
                "逐场证据在各场摘要。</p>\n";
    }

    // ---- 七 覆盖与限制 ----
    page << "<h2 id=\"sec-coverage\">七 覆盖与限制</h2>\n";
    {
        page << "<table class=\"facts\">\n"
             << "<tr><td>场次覆盖</td><td>found " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_found))
             << " · verified " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_verified))
             << " · analyzed " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_analyzed))
             << " · pending " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_pending))
             << " · excluded " << FormatNumber(static_cast<std::int64_t>(report.coverage.sessions_excluded))
             << "</td></tr>\n"
             << "<tr><td>usage 覆盖</td><td>" << FormatNumber(agg.requests_with_usage) << "/"
             << FormatNumber(agg.requests_total) << " 笔有 provider usage";
        if (agg.requests_unknown > 0) {
            page << ";已报请求合计见上表,另有 " << FormatNumber(agg.requests_unknown)
                 << " 笔 unknown,不估数补进实测总计";
        }
        page << "</td></tr>\n</table>\n";
        if (!extras.excluded.empty()) {
            page << "<table>\n<tr><th>排除场次</th><th>状态</th><th>理由</th></tr>\n";
            std::size_t shown = 0;
            for (const auto& entry : extras.excluded) {
                if (shown >= 50) {
                    break;
                }
                page << "<tr><td>" << EscapeHtml(entry.session_id) << "</td><td>"
                     << EscapeHtml(entry.status) << "</td><td>"
                     << EscapeHtml(entry.reason.empty() ? std::string("(未给理由)") : entry.reason)
                     << "</td></tr>\n";
                shown += 1;
            }
            page << "</table>\n";
            if (extras.excluded.size() > shown) {
                page << "<p class=\"muted\">…另有 " << (extras.excluded.size() - shown)
                     << " 场(排除场不进任何分母)。</p>\n";
            }
        }
        page << "<ul>\n"
             << "<li>active/corrupt/incomplete 场不悄悄混入完成率与 usage 分母,单列于上。</li>\n"
             << "<li>费用 not_priced:汇总层无逐模型拆账(A0 摘要 schema 未带),不拿总额估钱;"
                "逐笔贴价在 /usage。</li>\n"
             << "<li>选号按 session 开始时间从新到旧(结束时间不在 manifest,不猜)。</li>\n"
             << "<li>工作区只显 readable name 与 key 短码;绝对路径默认不进报告。</li>\n"
             << "<li>分析本地确定性,零模型请求;模型评议(--model-review)属后续批次 A6。</li>\n"
             << "<li>报告是派生物:删掉 derived 与 reports 后可从 Journal 重算,字节稳定。</li>\n";
        if (!extras.derived_errors.empty()) {
            page << "<li>有 " << extras.derived_errors.size()
                 << " 条摘要落盘失败(本地报告照出,长期摘要缺席,下轮会重算):";
            for (std::size_t i = 0; i < extras.derived_errors.size() && i < 5; ++i) {
                page << "<br><code>" << EscapeHtml(extras.derived_errors[i]) << "</code>";
            }
            page << "</li>\n";
        }
        page << "</ul>\n";
    }

    // ---- 场次明细(筛选面) ----
    page << "<h2 id=\"sec-sessions\">场次明细</h2>\n";
    {
        // 筛选维度的候选值:workspace/成色/outcome/摩擦类(全部来自报告数据)。
        std::set<std::string> workspaces, statuses, outcomes, categories;
        for (const auto& session : report.sessions) {
            const auto ws = extras.session_workspace.find(session.source.session_id);
            if (ws != extras.session_workspace.end()) {
                workspaces.insert(ws->second);
            }
            statuses.insert(session.source.integrity);
            if (!session.work.outcome.empty()) {
                outcomes.insert(session.work.outcome);
            }
            for (const auto& category : session.friction_events) {
                categories.insert(category);
            }
        }
        const auto options_for = [](const char* id, const std::set<std::string>& values,
                                    const char* label) {
            std::string out = "<label>" + std::string(label) + " <select id=\"" + id + "\">";
            out += "<option value=\"\">(全部)</option>";
            for (const auto& value : values) {
                out += "<option value=\"" + EscapeHtml(value) + "\">" + EscapeHtml(value) +
                       "</option>";
            }
            out += "</select></label>\n";
            return out;
        };
        page << "<div class=\"filters\">\n"
             << options_for("f-workspace", workspaces, "workspace")
             << options_for("f-status", statuses, "成色")
             << options_for("f-outcome", outcomes, "outcome")
             << options_for("f-category", categories, "摩擦类")
             << "<label>session <input type=\"text\" id=\"f-text\" placeholder=\"id 片段\"></label>\n"
             << "</div>\n"
             << "<p class=\"muted\">显示 <span id=\"session-visible\">0</span> / "
             << report.sessions.size() << " 场。model/purpose 不在摘要 schema 里,这两维不设筛"
                "(逐笔账在 /usage)。</p>\n"
             << "<table>\n<thead><tr><th>session</th><th>workspace</th><th>成色</th><th>turns</th>"
             << "<th>工具</th><th>输入</th><th>输出</th><th>outcome</th><th>摩擦类</th></tr></thead>\n"
             << "<tbody id=\"session-rows\">\n";
        for (const auto& session : report.sessions) {
            std::string cats;
            for (const auto& category : session.friction_events) {
                if (!cats.empty()) {
                    cats += " ";
                }
                cats += category;
            }
            std::string workspace;
            const auto ws = extras.session_workspace.find(session.source.session_id);
            if (ws != extras.session_workspace.end()) {
                workspace = ws->second;
            }
            page << "<tr data-session=\"" << EscapeHtml(session.source.session_id)
                 << "\" data-workspace=\"" << EscapeHtml(workspace) << "\" data-status=\""
                 << EscapeHtml(session.source.integrity) << "\" data-outcome=\""
                 << EscapeHtml(session.work.outcome) << "\" data-cats=\"" << EscapeHtml(cats)
                 << "\"><td><code>" << EscapeHtml(session.source.session_id) << "</code></td><td>"
                 << EscapeHtml(workspace) << "</td><td>" << EscapeHtml(session.source.integrity)
                 << "</td><td>" << FormatNumber(static_cast<std::int64_t>(session.work.turns))
                 << "</td><td>" << FormatNumber(static_cast<std::int64_t>(session.work.tool_calls))
                 << "</td><td>" << FormatTokens(session.usage.input_tokens) << "</td><td>"
                 << FormatTokens(session.usage.output_tokens) << "</td><td>"
                 << EscapeHtml(session.work.outcome.empty() ? std::string("-") : session.work.outcome)
                 << "</td><td>"
                 << EscapeHtml(cats.empty() ? std::string("-") : cats) << "</td></tr>\n";
        }
        page << "</tbody>\n</table>\n";
    }

    page << "<footer>本地确定性报告,零模型请求;token 与命中率为实测投影,金额未贴价。"
            "报告是派生物,可删可重算;实际账单看 provider。</footer>\n"
         << "</main>\n<script>" << kScript << "</script>\n</body>\n</html>\n";
    return page.str();
}

std::string InsightsHtmlSelfCheck() {
    // 带恶意样本文本渲染一遍:转义、CSP、七节锚点全要过。
    InsightsReport report;
    report.generated_at = "2026-08-31T00:00:00Z";
    report.scope.workspace_key = "selfcheck-000000000000";
    report.scope.since = "2026-08-01";
    report.scope.until = "2026-08-31";
    SessionInsightSummary session;
    session.source.session_id = "<script>alert(1)</script>";
    session.friction_events.push_back("cancelled</td></tr>");
    Finding finding;
    finding.finding_id = "P-AUD-SYNC";
    finding.category = "evil</table>";
    finding.summary = "</script><img src=x onerror=alert(2)>";
    session.prompt_findings.push_back(finding);
    report.sessions.push_back(session);
    const WorkspaceAggregate aggregate = AggregateInsights(report);
    InsightsRenderExtras extras;
    extras.workspace_names["selfcheck-000000000000"] = "selfcheck";
    extras.session_workspace[session.source.session_id] = "selfcheck-000000000000";
    const std::string html = RenderInsightsHtml(report, aggregate, extras);

    const auto contains = [&html](const std::string& needle) {
        return html.find(needle) != std::string::npos;
    };
    if (!contains("default-src 'none'")) {
        return "CSP 头缺 default-src 'none'";
    }
    if (!contains("script-src 'sha256-")) {
        return "CSP 头缺脚本哈希";
    }
    if (contains("<script>alert(1)</script>") || contains("</script><img")) {
        return "恶意样本未转义";
    }
    if (contains("http://") || contains("https://")) {
        return "页面带外链(应零网络)";
    }
    for (const char* anchor : {"sec-overview", "sec-usage", "sec-prompt", "sec-friction",
                               "sec-shape", "sec-suggestions", "sec-coverage"}) {
        if (!contains(anchor)) {
            return std::string("缺七节锚点: ") + anchor;
        }
    }
    return std::string();
}

}  // namespace lubancode::insights
