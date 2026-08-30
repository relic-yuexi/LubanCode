// Compact 四分区单·阶段 5:论文式多轮评测夹具(全假后端,不起真网)。
//
// 评测口径(两本账分家,验收线:能分别回答"省了多少 token"与"任务成功
// 率涨了多少",不混):
//   1. token 账——FULL(不压缩的完整上下文)、CONCAT(旧单发压缩:flat
//      存档 + 12k 热区)、turn 双账(新:双账 + 末分区热区)。同一份史
//      同一把尺(L1 工作视图压力口径),报节省比例与分布(P90-P10)。
//   2. 成功账——忠实模型下的约束保真:active 约束全保留、被纠正的旧约束
//      有来源地进 superseded、活动待办逐字守恒;另设两型坏模型(漏抄
//      要求/伪造来源)验证检测器咬得住。
//
// 诚实声明:这里量的是"管道保真度"(忠实模型下约束一字不丢)与 token
// 收益,不是真实模型的语义压缩质量——真机成功率须真模型真题另测,本
// 夹具不宣称"已解决多轮失败"。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/compact.hpp"
#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

// ---------------------------------------------------------------------------
// 夹具语言(评测双方——map 与 reduce 的模拟模型——都按它读材料):
//   "目标:<text>"        任务目标(首 turn)
//   "约束C<k>:<text>"    立约束 k
//   "改成C<k>:<text>"    纠正约束 k(旧文进 superseded)
//   "验收C<k>:<text>"    验收条件 k
//   "事实:<text>"        已证实事实
//   "文件:<path>"        涉及文件
//   "改动:<text>"        已做修改
//   "失败:<text>"        失败尝试
//   "待办:<text>"        未完成事项(活动待办逐字守恒面)
// map 产出的条目带 "tN|" 前缀钉 turn 归属(忠实模型数 chunk 里的用户轮,
// 与真实模型"按 turn 边界数轮"同一件事)。
// ---------------------------------------------------------------------------

api::Message UserText(const std::string& text) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantText(const std::string& text) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantToolUse(const std::string& id, const std::string& name) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::ToolUseBlock{id, name, nlohmann::json::object()});
    return m;
}

api::Message UserToolResult(const std::string& tool_use_id, const std::string& content) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::ToolResultBlock{tool_use_id, content, false});
    return m;
}

bool IsRealUserTurn(const api::Message& m) {
    if (m.role != api::Role::User) {
        return false;
    }
    for (const auto& block : m.content) {
        if (std::holds_alternative<api::TextBlock>(block) || std::holds_alternative<api::ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

// 一道评测题的形状参数(30 道由它枚举出来)。
struct EvalTask {
    std::size_t turn_count = 6;      // 4..8(2-8 turn 规格,压缩要有效至少 4)
    int constraint_keys = 2;         // 1..3
    std::vector<int> corrected;      // 哪些键会在后段被纠正
    std::vector<std::string> todos;  // 活动待办(逐字守恒面)
    bool has_tools = true;
    int pad = 0;                     // 抖动:正文长度
};

std::vector<api::Message> BuildHistory(const EvalTask& task) {
    std::vector<api::Message> history;
    const std::string pad(task.pad, 'x');
    int tool_seq = 0;
    for (std::size_t i = 0; i < task.turn_count; ++i) {
        std::string user_text;
        if (i == 0) {
            user_text += "目标:把多轮压缩评测做完\n";
        }
        // 立约束:前半段每 turn 立一枚键(结构化文本不带 pad——pad 是闲聊
        // 正文,只撑 token 体量,不进双账)。
        const int key = static_cast<int>(i) + 1;
        if (key <= task.constraint_keys && i < task.turn_count / 2) {
            user_text += "约束C" + std::to_string(key) + ":第" + std::to_string(key) + "号约束原文\n";
        }
        // 纠正:后半段逐轮轮换纠正 corrected 里的键(与立键的 turn 错开)。
        if (i >= task.turn_count / 2 && !task.corrected.empty()) {
            const int corr_key = task.corrected[i % task.corrected.size()];
            user_text += "改成C" + std::to_string(corr_key) + ":第" + std::to_string(corr_key) + "号约束改成新文\n";
        }
        if (i == task.turn_count - 1 && !task.todos.empty()) {
            for (const auto& todo : task.todos) {
                user_text += "待办:" + todo + "\n";
            }
        }
        user_text += "事实:第" + std::to_string(i + 1) + "轮证实的事实\n";
        user_text += "文件:src/eval_" + std::to_string(i) + ".cpp\n";
        user_text += "闲聊:" + pad + "\n";
        history.push_back(UserText(user_text));
        if (task.has_tools && i % 2 == 1) {
            ++tool_seq;
            history.push_back(AssistantToolUse("eval_tool_" + std::to_string(tool_seq), "read_file"));
            history.push_back(UserToolResult("eval_tool_" + std::to_string(tool_seq),
                                             "文件内容第" + std::to_string(tool_seq) + "份 " + std::string(9000, 'r')));
            history.push_back(AssistantText("改动:第" + std::to_string(i) + "轮的修改\n失败:第" +
                                            std::to_string(i) + "轮试错过一次"));
        } else {
            history.push_back(AssistantText("改动:第" + std::to_string(i) + "轮的修改\n失败:第" +
                                            std::to_string(i) + "轮试错过一次"));
        }
    }
    return history;
}

// ---------------------------------------------------------------------------
// 忠实模型(假后端):map 按 chunk 内容产 TurnGroupSummary;reduce 汇总
// summaries + 热区原文产双账。坏形注入由 flags 控制。
// ---------------------------------------------------------------------------

struct ModelFaults {
    int drop_constraint_key = -1;  // map 漏抄这枚键的约束(约束漏失检测)
    bool invent_source = false;    // reduce 伪造来源 turn(来源校验拒收检测)
    bool drop_todo = false;        // reduce 丢一条待办(守恒拒收检测)
};

class FaithfulEvalBackend : public api::Backend {
public:
    ModelFaults faults;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::string text =
            request.system.find("两份总账") != std::string::npos ? ReduceOutput(request) : MapOutput(request);
        on_event(api::MessageStart{"msg", "model"});
        on_event(api::TextDelta{text});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"end_turn", api::Usage{}});
        return {};
    }

private:
    static std::string BodyOf(const api::Request& request) {
        std::string body;
        for (const auto& message : request.messages) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                    body += text->text + "\n";
                }
            }
        }
        return body;
    }

    // 从 system 指令里抠 "来源 turn tA-tB" 的 A。
    static std::size_t RangeStartTurn(const std::string& system) {
        const std::size_t pos = system.find("来源 turn t");
        if (pos == std::string::npos) {
            return 1;
        }
        std::size_t value = 0;
        for (std::size_t i = pos + 11; i < system.size() && system[i] >= '0' && system[i] <= '9'; ++i) {
            value = value * 10 + static_cast<std::size_t>(system[i] - '0');
        }
        return value == 0 ? 1 : value;
    }

    std::string MapOutput(const api::Request& request) const {
        const std::size_t first_turn = RangeStartTurn(request.system);
        std::size_t turn_offset = 0;  // chunk 里第几枚用户轮(0 起)
        nlohmann::json summary;
        std::vector<std::string> changes;
        std::vector<std::string> facts;
        std::vector<nlohmann::json> tool_results;
        std::vector<std::string> files;
        std::vector<std::string> changes_made;
        std::vector<std::string> failed;
        std::vector<std::string> open_items;
        for (const auto& message : request.messages) {
            if (IsRealUserTurn(message) &&
                !std::holds_alternative<api::ToolResultBlock>(message.content[0])) {
                const std::string turn_id = "t" + std::to_string(first_turn + turn_offset);
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                        for (const std::string& line : SplitLines(text->text)) {
                            const std::string tagged = turn_id + "|" + line;
                            const std::size_t line_colon = line.find(':');
                            const std::string tail =
                                line_colon == std::string::npos ? line : line.substr(line_colon + 1);
                            if (line.rfind("约束C", 0) == 0 || line.rfind("改成C", 0) == 0 ||
                                line.rfind("目标:", 0) == 0) {
                                if (faults.drop_constraint_key >= 0 &&
                                    line.find("C" + std::to_string(faults.drop_constraint_key) + ":") !=
                                        std::string::npos) {
                                    continue;  // 坏模型:漏抄这枚键
                                }
                                changes.push_back(tagged);
                            } else if (line.rfind("事实:", 0) == 0) {
                                facts.push_back(tagged);
                            } else if (line.rfind("文件:", 0) == 0) {
                                files.push_back(tail);
                            } else if (line.rfind("待办:", 0) == 0) {
                                open_items.push_back(tagged);
                            }
                        }
                    }
                }
                ++turn_offset;
            } else if (message.role == api::Role::Assistant) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                        for (const std::string& line : SplitLines(text->text)) {
                            if (line.rfind("改动:", 0) == 0) {
                                changes_made.push_back(line.substr(3));
                            } else if (line.rfind("失败:", 0) == 0) {
                                failed.push_back(line.substr(3));
                            }
                        }
                    } else if (const auto* use = std::get_if<api::ToolUseBlock>(&block); use != nullptr) {
                        nlohmann::json item;
                        item["tool"] = use->name;
                        item["result"] = "关键读取(artifact 预览)";
                        item["evidence"] = "t" + std::to_string(first_turn + turn_offset) + ":e1";
                        tool_results.push_back(std::move(item));
                    }
                }
            }
        }
        summary["user_requirement_changes"] = changes;
        summary["confirmed_facts"] = facts;
        summary["tool_results"] = tool_results;
        summary["files"] = files;
        summary["changes_made"] = changes_made;
        summary["failed_attempts"] = failed;
        summary["open_items"] = open_items;
        summary["next_step_candidates"] = nlohmann::json::array({"评测收尾"});
        return summary.dump();
    }

    std::string ReduceOutput(const api::Request& request) const {
        const std::string body = BodyOf(request);
        // (turn 序号, 键, 文本, 是否纠正) 的时间线。
        struct Entry {
            std::size_t turn;
            std::string key;   // "C1" / "goal" / "fact..." / "todo..."
            std::string kind;  // goal/constraint/acceptance/fact/file/change/fail/todo
            std::string text;
            bool correction = false;
        };
        std::vector<Entry> timeline;
        auto absorb = [&timeline](const std::string& turn_and_line, std::size_t fallback_turn) {
            const std::size_t bar = turn_and_line.find('|');
            const std::string turn_text = bar == std::string::npos ? "" : turn_and_line.substr(0, bar);
            const std::string line = bar == std::string::npos ? turn_and_line : turn_and_line.substr(bar + 1);
            std::size_t turn = fallback_turn;
            if (turn_text.size() >= 2 && turn_text[0] == 't') {
                turn = static_cast<std::size_t>(std::atoi(turn_text.c_str() + 1));
            }
            Entry entry;
            entry.turn = turn == 0 ? fallback_turn : turn;
            // 注意:前缀都是"双字中文+半角冒号"(7 字节)或"约束C<k>:"(键在
            // 冒号前)——一律按冒号定位切,不按字节数硬切(UTF-8 多字节,
            // 切错半个字符会让 json dump 抛 invalid UTF-8)。
            const std::size_t colon = line.find(':');
            const std::string tail = colon == std::string::npos ? line : line.substr(colon + 1);
            if (line.rfind("目标:", 0) == 0) {
                entry.kind = "goal";
                entry.key = "goal";
                entry.text = tail;
            } else if (line.rfind("改成C", 0) == 0 || line.rfind("约束C", 0) == 0) {
                // 键 = "C<k>":前缀只数到中文两字(6 字节),C 留给键。
                const bool is_correction = line.rfind("改成C", 0) == 0;
                const std::size_t prefix_bytes = is_correction ? std::strlen("改成") : std::strlen("约束");
                entry.kind = "constraint";
                entry.key = line.substr(prefix_bytes, colon - prefix_bytes);
                entry.text = tail;
                entry.correction = is_correction;
            } else if (line.rfind("事实:", 0) == 0) {
                entry.kind = "fact";
                entry.key = "fact" + std::to_string(timeline.size());
                entry.text = tail;
            } else if (line.rfind("文件:", 0) == 0) {
                entry.kind = "file";
                entry.key = tail;
                entry.text = tail;
            } else if (line.rfind("改动:", 0) == 0) {
                entry.kind = "change";
                entry.key = "change" + std::to_string(timeline.size());
                entry.text = tail;
            } else if (line.rfind("失败:", 0) == 0) {
                entry.kind = "fail";
                entry.key = "fail" + std::to_string(timeline.size());
                entry.text = tail;
            } else if (line.rfind("待办:", 0) == 0) {
                entry.kind = "todo";
                entry.key = tail;
                entry.text = tail;
            } else {
                return;
            }
            timeline.push_back(std::move(entry));
        };

        // 材料一:局部小结 JSON(条目带 "tN|" 前缀)。
        std::size_t pos = 0;
        while (true) {
            const std::size_t header = body.find("=== 局部小结", pos);
            if (header == std::string::npos) {
                break;
            }
            const std::size_t line_begin = body.find('\n', header);
            const std::size_t line_end = body.find('\n', line_begin + 1);
            if (line_begin == std::string::npos || line_end == std::string::npos) {
                break;
            }
            const std::string json_line = body.substr(line_begin + 1, line_end - line_begin - 1);
            pos = line_end;
            const nlohmann::json parsed = nlohmann::json::parse(json_line, nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_object()) {
                continue;
            }
            for (const auto& item : parsed.value("user_requirement_changes", std::vector<std::string>{})) {
                absorb(item, 1);
            }
            for (const auto& item : parsed.value("confirmed_facts", std::vector<std::string>{})) {
                absorb(item, 1);
            }
            for (const auto& item : parsed.value("open_items", std::vector<std::string>{})) {
                absorb(item, 1);
            }
        }
        // 材料二:热区原文("--- turn tN · 用户 ---" 标号,归属精确)。
        {
            const std::size_t hot = body.find("最近热区原文");
            if (hot != std::string::npos) {
                std::size_t scan = hot;
                std::string current_turn;
                while (scan < body.size()) {
                    const std::size_t line_end = body.find('\n', scan);
                    const std::string line = body.substr(
                        scan, (line_end == std::string::npos ? body.size() : line_end) - scan);
                    if (line_end == std::string::npos) {
                        scan = body.size();
                    } else {
                        scan = line_end + 1;
                    }
                    const std::size_t marker = line.find("--- turn ");
                    if (marker != std::string::npos && line.find(" · 用户 ---") != std::string::npos) {
                        // "--- turn " 是 9 字节:turn 号从 marker+9 起。
                        const std::size_t turn_begin = marker + 9;
                        current_turn = line.substr(turn_begin, line.find(" · 用户 ---") - turn_begin);
                        continue;
                    }
                    if (line.rfind("--- ", 0) == 0) {
                        continue;  // 其他角色头或工具行
                    }
                    if (!current_turn.empty() &&
                        (line.rfind("约束C", 0) == 0 || line.rfind("改成C", 0) == 0 || line.rfind("目标:", 0) == 0 ||
                         line.rfind("事实:", 0) == 0 || line.rfind("文件:", 0) == 0 || line.rfind("待办:", 0) == 0)) {
                        absorb(current_turn + "|" + line, 1);
                    }
                }
            }
        }

        // 汇总:goal = 首条目标;constraint 键取时间线上最后一次出现的文本
        // (纠正后的新文),旧文进 superseded。
        nlohmann::json contract;
        std::string goal_text;
        std::string goal_turn = "t1";
        nlohmann::json active = nlohmann::json::array();
        nlohmann::json superseded = nlohmann::json::array();
        std::map<std::string, std::pair<std::string, std::size_t>> latest_of_key;      // 键 -> (文本, turn)
        std::map<std::string, std::pair<std::string, std::size_t>> original_of_key;    // 键 -> (旧文, turn)
        std::map<std::string, std::size_t> corrected_at;                               // 键 -> 纠正 turn
        for (const auto& entry : timeline) {
            if (entry.kind == "goal" && goal_text.empty()) {
                goal_text = entry.text;
                goal_turn = "t" + std::to_string(entry.turn);
            } else if (entry.kind == "constraint") {
                if (entry.correction) {
                    corrected_at[entry.key] = entry.turn;
                    latest_of_key[entry.key] = {entry.text, entry.turn};
                } else if (latest_of_key.count(entry.key) == 0 && corrected_at.count(entry.key) == 0) {
                    original_of_key[entry.key] = {entry.text, entry.turn};
                    latest_of_key[entry.key] = {entry.text, entry.turn};
                }
            }
        }
        for (const auto& [key, value] : latest_of_key) {
            nlohmann::json item;
            item["id"] = "r_" + key;
            item["text"] = value.first;
            const std::string source = faults.invent_source ? "t999" : "t" + std::to_string(value.second);
            item["source_turns"] = nlohmann::json::array({source});
            active.push_back(std::move(item));
        }
        for (const auto& [key, value] : original_of_key) {
            if (corrected_at.count(key) == 0) {
                continue;  // 没被纠正的键不进 superseded
            }
            nlohmann::json item;
            item["id"] = "s_" + key;
            item["text"] = value.first;
            item["source_turns"] = nlohmann::json::array({"t" + std::to_string(value.second)});
            item["superseded_by"] = "r_" + key;
            item["superseded_at_turn"] = "t" + std::to_string(corrected_at[key]);
            superseded.push_back(std::move(item));
        }
        contract["goal"] = {{"text", goal_text.empty() ? "评测目标" : goal_text},
                            {"source_turns", nlohmann::json::array({goal_turn})}};
        contract["active_constraints"] = active;
        contract["acceptance_criteria"] = nlohmann::json::array();
        contract["additions"] = nlohmann::json::array();
        contract["superseded_requirements"] = superseded;
        contract["open_questions"] = nlohmann::json::array();

        nlohmann::json state;
        nlohmann::json facts = nlohmann::json::array();
        std::set<std::string> files;
        nlohmann::json changes = nlohmann::json::array();
        nlohmann::json fails = nlohmann::json::array();
        nlohmann::json todos = nlohmann::json::array();
        std::size_t fact_seq = 0;
        for (const auto& entry : timeline) {
            const std::string ref = "t" + std::to_string(entry.turn);
            if (entry.kind == "fact") {
                facts.push_back({{"text", entry.text}, {"evidence_refs", nlohmann::json::array({ref})}});
                ++fact_seq;
            } else if (entry.kind == "file") {
                files.insert(entry.text);
            } else if (entry.kind == "change") {
                changes.push_back({{"text", entry.text}, {"evidence_refs", nlohmann::json::array({ref})}});
            } else if (entry.kind == "fail") {
                fails.push_back({{"text", entry.text}, {"evidence_refs", nlohmann::json::array({ref})}});
            } else if (entry.kind == "todo") {
                if (faults.drop_todo && todos.empty()) {
                    continue;  // 坏模型:丢第一条待办
                }
                todos.push_back(entry.text);
            }
        }
        state["confirmed_facts"] = facts;
        state["tool_results"] = nlohmann::json::array();
        state["files"] = std::vector<std::string>(files.begin(), files.end());
        state["changes_made"] = changes;
        state["failed_attempts"] = fails;
        state["open_items"] = todos;
        state["next_action"] = "评测收尾";
        (void)fact_seq;
        return nlohmann::json{{"user_contract", contract}, {"work_state", state}}.dump();
    }

    static std::vector<std::string> SplitLines(const std::string& text) {
        std::vector<std::string> lines;
        std::size_t begin = 0;
        while (begin <= text.size()) {
            const std::size_t end = text.find('\n', begin);
            if (end == std::string::npos) {
                if (begin < text.size()) {
                    lines.push_back(text.substr(begin));
                }
                break;
            }
            lines.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        return lines;
    }
};

// ---------------------------------------------------------------------------
// 评测指标
// ---------------------------------------------------------------------------

std::size_t PressureTokens(const std::vector<api::Message>& history) {
    agent::StructuralCompressionStats stats;
    return agent::EstimateHistoryTokens(
        agent::CompressWorkingView(history, agent::StructuralCompressionOptions{}, stats));
}

struct RunMetrics {
    std::size_t full_tokens = 0;       // FULL:不压缩的完整上下文
    std::size_t concat_tokens = 0;     // CONCAT:旧单发(flat 存档 + 12k 热区)
    std::size_t dual_tokens = 0;       // turn 双账新史
    std::size_t map_input_tokens = 0;  // 冷区重读成本(全部 map 请求输入)
    std::size_t map_calls = 0;
    double savings = 0.0;              // (full - dual) / full
    bool retained_all_active = false;  // 成功账:active 约束全保留
    bool superseded_correct = false;   // 被纠正旧约束有来源地进 superseded
    bool rejected = false;             // 校验拒收(坏模型路径预期为真)
    bool compact_ok = false;
};

RunMetrics RunOnce(const EvalTask& task, const ModelFaults& faults, std::string* failure_note = nullptr) {
    RunMetrics metrics;
    const std::vector<api::Message> history = BuildHistory(task);
    metrics.full_tokens = PressureTokens(history);

    // CONCAT 基线:旧单发压缩的形状(flat 存档 + 默认 12k 热区),存档体量
    // 用双账 JSON 同内容折算(同量对照,不虚报)。
    {
        api::Message flat = UserText("[对话存档,此前内容已压缩] ## 任务目标\n评测目标\n## 未完成事项\n" +
                                     std::string(600, 's'));
        const auto concat_history = agent::BuildCompactedHistory(history, flat, agent::kDefaultHotZoneTokens);
        metrics.concat_tokens = PressureTokens(concat_history);
    }

    agent::CompactOptions options;
    options.partition_count = 4;
    for (const auto& todo : task.todos) {
        options.required_open_items.push_back(todo);
    }
    FaithfulEvalBackend backend;
    backend.faults = faults;
    const auto result = agent::CompactTurnPartitioned(backend, "eval-model", history, options,
                                                      agent::StructuralCompressionOptions{});
    if (!result.has_value()) {
        metrics.rejected = true;
        if (failure_note != nullptr) {
            *failure_note = result.error().message;
        }
        return metrics;
    }
    metrics.compact_ok = true;
    metrics.dual_tokens = PressureTokens(result->new_history);
    metrics.map_calls = result->metrics.chunks;
    for (std::size_t i = 0; i + 1 < backend.captured_requests.size(); ++i) {
        metrics.map_input_tokens += agent::EstimateHistoryTokens(backend.captured_requests[i].messages) +
                                    agent::EstimateUtf8Tokens(backend.captured_requests[i].system);
    }
    if (metrics.full_tokens > 0) {
        metrics.savings = static_cast<double>(metrics.full_tokens - metrics.dual_tokens) /
                          static_cast<double>(metrics.full_tokens);
    }

    // 成功账:期望的 active 约束(每键最新文)全在;被纠正的键 superseded 正确。
    bool all_active = true;
    bool all_superseded = true;
    // 期望值从夹具重推(与 BuildHistory 同一套算术)。
    for (int key = 1; key <= task.constraint_keys; ++key) {
        const bool corrected = std::find(task.corrected.begin(), task.corrected.end(), key) != task.corrected.end();
        const std::string expected = corrected ? "第" + std::to_string(key) + "号约束改成新文"
                                               : "第" + std::to_string(key) + "号约束原文";
        bool found = false;
        for (const auto& requirement : result->contract.active_constraints) {
            if (requirement.text.find(expected) != std::string::npos) {
                found = true;
            }
        }
        if (!found) {
            all_active = false;
        }
        if (corrected) {
            bool superseded_found = false;
            for (const auto& old : result->contract.superseded_requirements) {
                if (old.text.find("第" + std::to_string(key) + "号约束原文") != std::string::npos &&
                    old.superseded_by == "r_C" + std::to_string(key)) {
                    superseded_found = true;
                }
            }
            if (!superseded_found) {
                all_superseded = false;
            }
        }
    }
    metrics.retained_all_active = all_active;
    metrics.superseded_correct = all_superseded;
    if (std::getenv("LUBANCODE_COMPACT_EVAL_VERBOSE") != nullptr && (!all_active || !all_superseded)) {
        std::cout << "[compact-eval-miss] retained=" << all_active << " superseded=" << all_superseded
                  << " corrected_keys=";
        for (const auto& key : task.corrected) {
            std::cout << key << ",";
        }
        std::cout << " active_size=" << result->contract.active_constraints.size()
                  << " superseded_size=" << result->contract.superseded_requirements.size();
        for (const auto& old : result->contract.superseded_requirements) {
            std::cout << " [sup " << old.id << " by " << old.superseded_by << " text~" << old.text.substr(0, 12)
                      << "]";
        }
        std::cout << "\n";
    }
    return metrics;
}

std::vector<EvalTask> BuildTaskSet() {
    std::vector<EvalTask> tasks;
    const std::size_t turn_counts[] = {4, 5, 6, 7, 8};
    for (std::size_t i = 0; i < 30; ++i) {
        EvalTask task;
        task.turn_count = turn_counts[i % 5];
        // 立键只发生在前半段:键数不得超过 turn_count/2,否则后半段没有
        // 可纠正的原文(期望值推导与 BuildHistory 同一套算术)。
        const int max_keys = static_cast<int>(task.turn_count / 2);
        task.constraint_keys = std::min(1 + static_cast<int>(i % 3), max_keys);
        if (i % 4 == 0) {
            task.corrected.push_back(1);  // C1 被纠正(纠正轮在后半段)
        }
        if (i % 7 == 0 && task.constraint_keys >= 2) {
            task.corrected.push_back(2);
        }
        task.todos.push_back("评测待办甲" + std::to_string(i));
        if (i % 3 == 0) {
            task.todos.push_back("评测待办乙" + std::to_string(i));
        }
        task.has_tools = i % 5 != 4;  // 五分之一纯对话题
        task.pad = 1200 + static_cast<int>((i * 37) % 800);
        tasks.push_back(task);
    }
    return tasks;
}

double Percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}

}  // namespace

// ---------------------------------------------------------------------------
// token 账与成功账分开断言;P90-P10 分布、均值一张表。
// ---------------------------------------------------------------------------

TEST_CASE("阶段 5 评测: 30 题 × 10 次忠实模型——token 账与成功账两本分开") {
    const std::vector<EvalTask> tasks = BuildTaskSet();
    REQUIRE(tasks.size() == 30);

    std::vector<double> savings_runs;
    std::size_t total_runs = 0;
    std::size_t ok_runs = 0;
    std::size_t retained_runs = 0;
    std::size_t superseded_runs = 0;
    std::size_t dual_beats_full_runs = 0;
    std::size_t map_calls_total = 0;
    std::size_t map_calls_expected = 0;  // 预算内固定 = 分区数 - 1(每题 3)

    for (std::size_t seed = 0; seed < 10; ++seed) {
        for (std::size_t t = 0; t < tasks.size(); ++t) {
            EvalTask task = tasks[t];
            task.pad += static_cast<int>(seed * 13);  // 抖动:正文长度
            const ModelFaults clean;
            std::string note;
            const RunMetrics metrics = RunOnce(task, clean, &note);
            ++total_runs;
            if (!metrics.compact_ok) {
                if (std::getenv("LUBANCODE_COMPACT_EVAL_VERBOSE") != nullptr) {
                    std::cout << "[compact-eval-reject] task=" << t << " seed=" << seed << " note=" << note << "\n";
                }
            }
            if (metrics.compact_ok) {
                ++ok_runs;
                savings_runs.push_back(metrics.savings);
                map_calls_total += metrics.map_calls;
                map_calls_expected += 3;  // 30 题都 ≥4 turn:四分区预算内固定 3 次 map
                if (metrics.retained_all_active) {
                    ++retained_runs;
                }
                if (metrics.superseded_correct) {
                    ++superseded_runs;
                }
                if (metrics.dual_tokens < metrics.full_tokens) {
                    ++dual_beats_full_runs;
                }
            }
        }
    }

    REQUIRE(ok_runs == total_runs);  // 忠实模型下一次不拒

    // ---- token 账 ----
    const double mean_savings = [&] {
        double sum = 0.0;
        for (const double value : savings_runs) {
            sum += value;
        }
        return sum / static_cast<double>(savings_runs.size());
    }();
    const double p90 = Percentile(savings_runs, 0.9);
    const double p10 = Percentile(savings_runs, 0.1);
    CHECK(mean_savings > 0.5);            // 平均省一半以上(热区=1/4,冷区全收进双账)
    CHECK(dual_beats_full_runs == total_runs);  // 每一场都比 FULL 短
    CHECK(p10 > 0.2);                     // 最差的一场也省两成以上
    CHECK(map_calls_total == map_calls_expected);  // 预算内 map 调用数固定 3

    // ---- 成功账(保真度) ----
    CHECK(retained_runs == total_runs);   // active 约束无一漏
    CHECK(superseded_runs == total_runs); // 被纠正旧约束全部有来源地进 superseded

    // 汇总一行(测试输出里可查)。
    std::cout << "[compact-eval] runs=" << total_runs << " mean_savings=" << mean_savings
              << " p90=" << p90 << " p10=" << p10 << " retention=" << retained_runs << "/" << total_runs << "\n";
}

TEST_CASE("阶段 5 评测: 坏模型三型——漏抄/伪造来源/丢待办,检测器都咬得住") {
    const std::vector<EvalTask> tasks = BuildTaskSet();

    // 漏抄约束:map 不写 C2(C2 只在冷区 turn 2 出现,热区救不回),最终
    // 契约必缺 C2 的原文(约束漏失可测)。
    {
        ModelFaults faults;
        faults.drop_constraint_key = 2;
        const RunMetrics metrics = RunOnce(tasks[1], faults);  // tasks[1]:C1/C2 都在冷区
        REQUIRE(metrics.compact_ok);
        CHECK_FALSE(metrics.retained_all_active);
    }
    // 伪造来源 turn:校验拒收,旧史不动。
    {
        ModelFaults faults;
        faults.invent_source = true;
        const RunMetrics metrics = RunOnce(tasks[1], faults);
        CHECK(metrics.rejected);
        CHECK_FALSE(metrics.compact_ok);
    }
    // 丢待办:守恒校验拒收。
    {
        ModelFaults faults;
        faults.drop_todo = true;
        const RunMetrics metrics = RunOnce(tasks[3], faults);  // tasks[3] 带两条待办
        CHECK(metrics.rejected);
        CHECK_FALSE(metrics.compact_ok);
    }
}

TEST_CASE("阶段 5 评测: 工具重载题的 map 输入按工作视图计量(长结果不虚算)") {
    EvalTask task;
    task.turn_count = 8;
    task.constraint_keys = 2;
    task.corrected = {1};
    task.todos = {"重载待办"};
    task.has_tools = true;
    task.pad = 300;

    const RunMetrics metrics = RunOnce(task, ModelFaults{});
    REQUIRE(metrics.compact_ok);
    // map 重读账(§6.4):约等于冷区工作视图 + 每块指令开销——冷区长结果已
    // 按 artifact 预览计量,重读一遍(加指令)仍远小于"全量原文读两遍"。
    CHECK(metrics.map_input_tokens < metrics.full_tokens * 2);
    CHECK(metrics.map_calls == 3);
    CHECK(metrics.retained_all_active);
    CHECK(metrics.superseded_correct);
}
