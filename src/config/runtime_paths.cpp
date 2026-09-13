// runtime_paths.hpp 的实现。纯解析 + 一次性的文件系统可达性探测;
// 不缓存——启动门调一次,库级消费走 HomeLubancodeDir()/StateRootDir()
// (config.cpp),它们各自现读 env,与本文件同一套取值口径。
#include "config/runtime_paths.hpp"

#include <system_error>

#include "platform/paths.hpp"

namespace lubancode::config {

std::optional<std::string> RuntimeEnvSnapshot::Get(const char* name) const {
    const auto it = vars.find(name);
    if (it == vars.end()) {
        return std::nullopt;
    }
    return it->second;
}

RuntimeEnvSnapshot CaptureProcessEnv() {
    RuntimeEnvSnapshot env;
    for (const char* name : {"LUBANCODE_HOME", "LUBANCODE_DATA_HOME", "LUBANCODE_MANAGED"}) {
        // GetEnvVarPresent:空串也算"设了"——空值是配置错误,不是没设。
        if (auto value = platform::GetEnvVarPresent(name)) {
            env.vars.emplace(name, *value);
        }
    }
    env.home_dir = platform::HomeDir();
    return env;
}

std::expected<RuntimePaths, std::string> ResolveRuntimePaths(const RuntimeEnvSnapshot& env) {
    RuntimePaths out;
    const auto home_raw = env.Get("LUBANCODE_HOME");
    const auto data_raw = env.Get("LUBANCODE_DATA_HOME");
    const auto managed_raw = env.Get("LUBANCODE_MANAGED");

    // LUBANCODE_MANAGED:只认 1(开)与 0(关),空串与别的值不猜——这枚
    // 变量管的是整档来源裁剪,静默当 0 会让部署者以为托管生效了。
    if (managed_raw.has_value()) {
        if (*managed_raw == "1") {
            out.managed = true;
        } else if (*managed_raw == "0") {
            out.managed = false;
        } else {
            return std::unexpected("LUBANCODE_MANAGED 只认 1 或 0,写的是: \"" + *managed_raw +
                                   "\"(空串也不行,要关就写 0 或整只删掉)");
        }
    }

    // 托管模式必须显式给参数根:托管的第一件事就是在参数根里发现文件、
    // 播种默认材料(单 §4.1"先识别,再发现文件、播种默认材料"),没根的
    // 托管无从谈起,也不许回落个人目录顶替。
    if (out.managed && !home_raw.has_value()) {
        return std::unexpected(
            "LUBANCODE_MANAGED=1 托管模式必须显式设置 LUBANCODE_HOME(参数根);"
            "不设根的托管没有材料来源,也不回落个人 ~/.lubancode");
    }

    // 未启用应用根:个人 CLI 旧布局,原合同不动。孤立的 DATA_HOME 是配置
    // 错误——静默忽略会让人以为数据根生效了,实际状态还写在个人目录。
    if (!home_raw.has_value()) {
        if (data_raw.has_value()) {
            return std::unexpected(
                "LUBANCODE_DATA_HOME 不能脱离 LUBANCODE_HOME 单独设置;"
                "未启用应用根时旧 CLI 布局不认数据根(要用请一并设置 LUBANCODE_HOME)");
        }
        return out;
    }

    // 参数根:非空绝对路径,值即根本身(不追加 .lubancode)。
    if (home_raw->empty()) {
        return std::unexpected(
            "LUBANCODE_HOME 不能设为空串;要回个人默认布局请整只删掉这个变量,"
            "空值不等于未设置");
    }
    const std::filesystem::path home_path = platform::Utf8ToPath(*home_raw);
    if (!home_path.is_absolute()) {
        return std::unexpected("LUBANCODE_HOME 必须是绝对路径,写的是: " + *home_raw +
                               "(相对组件路径按声明文件所在目录解析,根不行)");
    }
    out.app_root_active = true;
    out.config_root = home_path;

    // 数据根:显式给则过同一套校验;缺省 <HOME>/data(参数根内子目录,
    // 合法重叠——参数根只读挂载时应显式给 LUBANCODE_DATA_HOME)。
    std::filesystem::path data_path;
    if (data_raw.has_value()) {
        if (data_raw->empty()) {
            return std::unexpected("LUBANCODE_DATA_HOME 不能设为空串");
        }
        data_path = platform::Utf8ToPath(*data_raw);
        if (!data_path.is_absolute()) {
            return std::unexpected("LUBANCODE_DATA_HOME 必须是绝对路径,写的是: " + *data_raw);
        }
    } else {
        data_path = home_path / "data";
    }

    // 重叠裁决:数据根==参数根(读写不分)或参数根在数据根之内(只读根
    // 被可写根吞)都拒;数据根在参数根之内合法(默认即如此)。比较走
    // PathComparisonKey(weakly_canonical 优先),同一条路径两种写法
    //(斜杠/大小写/链接)算同一地方。
    const std::string data_key = platform::PathComparisonKey(data_path);
    const std::string home_key = platform::PathComparisonKey(home_path);
    if (data_key == home_key) {
        return std::unexpected(
            "LUBANCODE_DATA_HOME 不能等于 LUBANCODE_HOME:参数根(可只读)与"
            "数据根(可写)必须分开,状态直写参数根会把只读挂载顶破");
    }
    if (home_key.rfind(data_key + "/", 0) == 0) {
        return std::unexpected(
            "LUBANCODE_DATA_HOME 不能包住 LUBANCODE_HOME:参数根落进可写数据根"
            "之内,只读挂载无从谈起(默认 <HOME>/data 是数据根在参数根内,合法)");
    }
    out.data_root = data_path;
    return out;
}

std::expected<void, std::string> EnsureRuntimeRootsAccessible(const RuntimePaths& paths) {
    if (!paths.app_root_active) {
        return {};  // 个人布局不动——旧目录结构的建立归各消费方原逻辑
    }
    namespace fs = std::filesystem;
    for (const auto* root : {&paths.config_root, &paths.data_root}) {
        if (!root->has_value()) {
            continue;
        }
        std::error_code ec;
        // 已存在(目录或链接)不算错;建不动(父目录无权限/盘满)才报。
        // 缺失即建:参数根由托管播种/应用部署落材料,数据根按合同
        // "合法但缺失的数据目录在获准父目录内创建"。
        if (!fs::exists(**root, ec) && !ec) {
            ec.clear();
            fs::create_directories(**root, ec);
        }
        if (ec) {
            return std::unexpected("根目录不可用(建不动): " + platform::PathToUtf8(**root) +
                                   " — " + ec.message());
        }
        if (!fs::is_directory(**root, ec) && !ec) {
            return std::unexpected("根路径不是目录: " + platform::PathToUtf8(**root));
        }
    }
    return {};
}

}  // namespace lubancode::config
