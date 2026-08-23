// undo_file_edit 的实现(逐枚追踪单第四期:条件式撤销的执行侧)。

#include "tools/undo_file_edit.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "hooks/hash.hpp"  // Sha256Hex:当前内容与 token 的 postimage 对账
#include "platform/text_encoding.hpp"
#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"

namespace lubancode::tools {

std::string UndoFileEditTool::name() const {
    return "undo_file_edit";
}

std::string UndoFileEditTool::description() const {
    // 文案在 src/prompts/tools/<语言>/undo_file_edit.md,兜底是这里的原文。
    return ToolText(
        "undo_file_edit", "description",
        "撤销此前一次 write_file 或 edit_file 对某个文件的改动(条件式:只有该文件在改动之后没被再改过才"
        "能撤销)。入参给那次工具调用的 execution_id(从 /trace 查得)。改动后文件又被改过时本工具不会"
        "自动撤销,而是给出三方对比交人处置。执行前需要用户确认。");
}

nlohmann::json UndoFileEditTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json exec_prop = nlohmann::json::object();
    exec_prop["type"] = "string";
    exec_prop["description"] = ToolText("undo_file_edit", "param.execution_id",
                                        "要撤销的那次 write_file/edit_file 调用的 execution_id(/trace 可查)");
    properties["execution_id"] = exec_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"execution_id"});
    return schema;
}

Tool::Result UndoFileEditTool::execute(const nlohmann::json& input) {
    last_compensates_.clear();
    if (!input.contains("execution_id") || !input.at("execution_id").is_string()) {
        return {"缺少必填参数 execution_id(字符串)", true};
    }
    const std::string execution_id = input.at("execution_id").get<std::string>();
    if (execution_id.empty()) {
        return {"execution_id 不能是空字符串", true};
    }
    if (!lookup_.alive()) {
        return {"本会话没有可用的工具追踪账,查不到这枚调用的撤销凭据。", true};
    }
    // compensates 关系先记:这枚 undo 想补偿的是 execution_id 指的那枚。
    // 关系边是"意图",不是"成功"——补偿失败的账也要带边(单子:"补偿
    // 失败也须留账"),所以放在查表之前,早退路径同样有边。owner 查得
    // 到用 owner,查不到退回 execution_id 本身。
    last_compensates_ = lookup_.owner_of ? lookup_.owner_of(execution_id) : execution_id;
    if (last_compensates_.empty()) {
        last_compensates_ = execution_id;
    }
    const auto token = lookup_.find(execution_id);
    if (!token.has_value() || token->path.empty()) {
        return {"追踪账里没有这枚 execution 的撤销凭据(可能是只读调用、旧档没带 token、或 preimage 过大"
                "未随账保留)。文件级回退请走 Git/worktree 检查点。",
                true};
    }

    // 隔离文件闸:与 write/edit 同一道门(undo 也是写,不许越过房界)。
    if (const IsolationScope* scope = IsolationGuard::Current();
        scope != nullptr && PathBlockedByIsolation(token->path, *scope)) {
        return {"[隔离] 会话正住在 worktree " + scope->name + " 里,不许改主 checkout 的文件: " + token->path +
                    "。请在房内操作,或先 worktree exit 出房。",
                true};
    }

    const auto outcome = ApplyConditionalUndo(Utf8ToPath(token->path), *token);

    Tool::Result result{outcome.message, outcome.is_error};
    result.outcome = outcome.performed ? "succeeded" : (outcome.is_error ? "tool_error" : "succeeded");
    result.effect_summary = std::string("undo ") + token->path + (outcome.performed ? " (performed)" : " (refused)");
    if (outcome.performed) {
        // undo 本身也产 token:撤销后的内容回到 preimage,这一枚的
        // postimage 是 preimage(再来一次 undo 就能 redo——方向翻转,
        // 关系靠 compensates 串成链,不靠 token 自指)。
        result.undo_path = token->path;
        result.undo_preimage_sha256 = token->postimage_sha256;
        result.undo_postimage_sha256 = token->preimage_sha256;
        result.undo_preimage = token->preimage;
        result.undo_created_new_file = false;
    }
    return result;
}

namespace {

// 短头部预览:三方对比用的行截断(给模型/用户看,不承担恢复)。
std::string HeadPreview(const std::string& text, std::size_t cap) {
    if (text.size() <= cap) {
        return text;
    }
    return text.substr(0, cap) + "…(共 " + std::to_string(text.size()) + " 字节)";
}

}  // namespace

ConditionalUndoOutcome ApplyConditionalUndo(const std::filesystem::path& path, const agent::ToolUndoToken& token) {
    ConditionalUndoOutcome out;

    // token 不可用:旧文件要恢复 preimage,preimage 没随账保留(超限)
    // 就无从恢复——如实说不撤销,指路 Git。新建文件的移走只核对
    // postimage,不须 preimage,放行。
    if (!token.created_new_file && !token.available()) {
        out.is_error = true;
        out.message = "撤销凭据不完整(preimage 未随账保留,通常是那次改动超过内联上限)。"
                      "不自动撤销;文件级回退请走 Git diff/commit 或 worktree 检查点。";
        return out;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // 文件没了:新建文件被删过/旧文件被移走,都不是"内容仍等于
        // postimage",拒绝;但如实说明现场。
        out.is_error = true;
        out.message = "目标文件已不存在(" + PathToUtf8(path) + "),无从核对改动后状态,不自动撤销。";
        return out;
    }

    std::string current;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            out.is_error = true;
            out.message = "打不开目标文件核对内容(权限不够或被占用): " + PathToUtf8(path);
            return out;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        current = buffer.str();
    }

    const std::string current_sha = hooks::Sha256Hex(current);
    const std::string post_sha = token.postimage_sha256.empty() ? hooks::Sha256Hex(std::string{})
                                                                : token.postimage_sha256;

    if (current_sha != post_sha) {
        // 其后有人再改:拒绝自动撤销,给三方 diff 交用户(单子原文)。
        out.is_error = true;
        std::ostringstream msg;
        msg << "目标文件在改动之后又被改过(当前内容与撤销凭据的 postimage 不符),不自动撤销。\n"
            << "三方对照(请人工取舍):\n"
            << "--- 改动前(preimage," << token.preimage.size() << " 字节)---\n"
            << HeadPreview(token.preimage, 2000) << "\n"
            << "+++ 改动后(postimage)与当前内容不同;当前内容(" << current.size() << " 字节)---\n"
            << HeadPreview(current, 2000) << "\n";
        out.message = msg.str();
        return out;
    }

    if (token.created_new_file) {
        // 新建文件:内容仍等于 postimage,可移走(删文件)。
        std::ifstream holder(path, std::ios::binary);
        holder.close();  // 先关柄再删(Windows 的雷)
        std::error_code remove_ec;
        if (!std::filesystem::remove(path, remove_ec)) {
            out.is_error = true;
            out.message = "新建文件已核对一致,但删除失败: " + remove_ec.message();
            return out;
        }
        out.performed = true;
        out.message = "已移走新建文件(内容与撤销凭据一致): " + PathToUtf8(path);
        return out;
    }

    // 旧文件:恢复 preimage。
    std::ofstream out_file(path, std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
        out.is_error = true;
        out.message = "打不开目标文件写回(权限不够或被占用): " + PathToUtf8(path);
        return out;
    }
    out_file.write(token.preimage.data(), static_cast<std::streamsize>(token.preimage.size()));
    if (!out_file) {
        out.is_error = true;
        out.message = "写回 preimage 失败: " + PathToUtf8(path);
        return out;
    }
    out_file.close();
    out.performed = true;
    out.message = "已恢复改动前内容(其间无人再改,核对通过): " + PathToUtf8(path) + " (" +
                  std::to_string(token.preimage.size()) + " 字节)";
    return out;
}

}  // namespace lubancode::tools
