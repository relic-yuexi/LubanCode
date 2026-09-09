#include "tools/write_file.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "hooks/hash.hpp"  // Sha256Hex:undo token 的 pre/post image 摘要
#include "platform/atomic_write.hpp"  // 崩溃级原子落盘:临时文件写全 + 原子换名
#include "platform/text_encoding.hpp"
#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

std::string WriteFileTool::name() const {
    return "write_file";
}

std::string WriteFileTool::description() const {
    // 文案在 src/prompts/tools/<语言>/write_file.md,兜底是迁移前的原文。
    return ToolText("write_file", "description",
                    "把内容写入文件(UTF-8 编码)。文件已存在就整个覆盖,父目录不存在会自动建好。"
                    "路径可以是相对路径,也可以是绝对路径。适合新建文件或整篇重写;小范围改动用 "
                    "edit_file 更精准。执行前需要用户确认。");
}

nlohmann::json WriteFileTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = ToolText("write_file", "param.path", "要写入的文件路径,相对或绝对均可");
    properties["path"] = path_prop;

    nlohmann::json content_prop = nlohmann::json::object();
    content_prop["type"] = "string";
    content_prop["description"] =
        ToolText("write_file", "param.content", "要写入的文件内容(UTF-8),会整体覆盖原文件");
    properties["content"] = content_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"path", "content"});

    return schema;
}

Tool::Result WriteFileTool::execute(const nlohmann::json& input) {
    if (!input.contains("path") || !input.at("path").is_string()) {
        return {"缺少必填参数 path(字符串)", true};
    }
    if (!input.contains("content") || !input.at("content").is_string()) {
        return {"缺少必填参数 content(字符串)", true};
    }
    const std::string path_str = input.at("path").get<std::string>();
    if (path_str.empty()) {
        return {"path 不能是空字符串", true};
    }
    const std::string content = input.at("content").get<std::string>();

    // 隔离文件闸:住在 worktree 房里的会话/子代理,写主 checkout 的文件一律拦。
    if (const IsolationScope* scope = IsolationGuard::Current();
        scope != nullptr && PathBlockedByIsolation(path_str, *scope)) {
        return {"[隔离] 会话正住在 worktree " + scope->name + " 里,不许写主 checkout 的文件: " + path_str +
                    "。请在房内操作,或先 worktree exit 出房。",
                true};
    }

    const std::filesystem::path path = Utf8ToPath(path_str);

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return {"这是个目录,不能当文件写: " + path_str, true};
    }
    const bool existed_before = std::filesystem::exists(path, ec);

    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return {"建父目录失败: " + ec.message(), true};
        }
    }

    // 逐枚追踪单:preimage 先读(条件式撤销的 token 靠它——当前内容仍
    // 等于 postimage 才可恢复 preimage;新建文件只在内容未再变时可移走)。
    std::string preimage;
    if (existed_before) {
        std::ifstream in(path, std::ios::binary);
        if (in.is_open()) {
            std::ostringstream buffer;
            buffer << in.rdbuf();
            preimage = buffer.str();
        }
    }

    // [崩溃级原子·write_file 原子写单] 落盘改走 platform::AtomicWriteFile:
    // 先把整份内容写进临时文件,再原子换名(Windows MoveFileExW
    // REPLACE_EXISTING|WRITE_THROUGH / POSIX rename)。任何时点硬崩/断电,
    // 目标要么旧整份、要么新整份,绝无半截;新建文件同享此合同——不写
    // 成,目标压根不出现,不留零字节残尸(事故现场那只空 todo 文件,正是
    // open(trunc) 先截后写夹在中间的形状)。
    // 临时件规矩归基建:落目标同目录(同卷才原子),名如 <目标>.<pid>-
    // <序号>.tmp,失败路径删净;硬崩孤儿留在同目录,后缀可辨、无主无害,
    // 本工具不越权收走别人的 tmp。档位取默认 AtomicVisibility:单子合同
    // 是"非旧即新",进程崩后换名已在页缓存生效,目标即新文;断电至多翻
    // 回旧文,仍非半截——要"成功必耐断电"再升 ProcessCrashDurability
    // (fsync 档),不在本单。取消语义照旧协作式盲(工具体不读 cancel 旗,
    // ESC 掐不进写盘),由下方取消盲测钉住。
    const auto written = platform::AtomicWriteFile(path, content);
    if (!written.has_value()) {
        // 文案口径沿用 ofstream 时代:开不成/写不下两句原样;换名不成是
        // 新通路,照同款人话补——三路失败目标都未动。
        const std::string& code = written.error().code;
        if (code == "atomic.tmp_open_failed") {
            return {"打不开文件写(权限不够或者路径不对): " + path_str, true};
        }
        if (code == "atomic.replace_failed") {
            return {"写文件失败(临时文件换名没换成,原文件保持原样): " + path_str, true};
        }
        return {"写文件失败: " + path_str, true};
    }

    // undo token(逐枚追踪单"本地文件条件式撤销"):超 kUndoPreimageCap
    // 不内联正文,token 标不可用——不拿半截原文冒充可恢复。
    Tool::Result result;
    result.SetText("写入成功,共 " + std::to_string(content.size()) + " 字节: " + path_str);
    if (existed_before) {
        result.AppendText("(覆盖了原有文件)");
    }
    result.undo_path = path_str;
    result.undo_preimage_sha256 = hooks::Sha256Hex(preimage);
    result.undo_postimage_sha256 = hooks::Sha256Hex(content);
    result.undo_created_new_file = !existed_before;
    if (static_cast<std::uint64_t>(preimage.size()) <= Tool::kToolUndoPreimageCap) {
        result.undo_preimage = std::move(preimage);
    }
    result.effect_summary = "write " + path_str + " (" + std::to_string(content.size()) + " bytes)";
    return result;
}

}  // namespace lubancode::tools
