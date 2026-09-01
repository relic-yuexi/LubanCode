#include "cli/slash_commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 从 pos 开始跳过前导空白,取一个"非空白游程"当一个词,pos 挪到词结束后
// 的位置(可能是空白,也可能是字符串末尾)。取不到词(到头了)返回
// std::nullopt,pos 保持不变。跟 istringstream 的 >> 效果一样,但不用忍受
// streampos/tellg() 的边界怪癖,而且用完之后 s.substr(pos) 就是"剩下的
// 原始文本"——extra_body/extra_header 的值就靠这个拿。
std::optional<std::string> NextToken(const std::string& s, std::size_t& pos) {
    std::size_t i = pos;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) {
        ++i;
    }
    if (i >= s.size()) {
        return std::nullopt;
    }
    std::size_t j = i;
    while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j])) == 0) {
        ++j;
    }
    pos = j;
    return s.substr(i, j - i);
}

}  // namespace

ParsedSlashCommand ParseSlashCommand(const std::string& input) {
    ParsedSlashCommand parsed;

    const std::string trimmed = Trim(input);
    if (trimmed.empty() || trimmed[0] != '/') {
        parsed.command = SlashCommand::NotSlash;
        return parsed;
    }

    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string word = (space == std::string::npos) ? trimmed : trimmed.substr(0, space);
    const std::string args = (space == std::string::npos) ? "" : Trim(trimmed.substr(space + 1));

    parsed.raw_word = word;
    parsed.args = args;

    const std::string lower = ToLower(word);
    if (lower == "/help") {
        parsed.command = SlashCommand::Help;
    } else if (lower == "/model") {
        parsed.command = SlashCommand::Model;
    } else if (lower == "/provider") {
        parsed.command = SlashCommand::Provider;
    } else if (lower == "/config") {
        parsed.command = SlashCommand::Config;
    } else if (lower == "/update") {
        parsed.command = SlashCommand::Update;
    } else if (lower == "/init") {
        parsed.command = SlashCommand::Init;
    } else if (lower == "/clear") {
        parsed.command = SlashCommand::Clear;
    } else if (lower == "/exit" || lower == "/quit") {
        parsed.command = SlashCommand::Exit;
    } else if (lower == "/context") {
        parsed.command = SlashCommand::Context;
    } else if (lower == "/usage") {
        // Token 账本单 A2:/usage 只认词,二级参数(session/--by/--json)在
        // ParseUsageCommand(app/commands/usage_commands)拆。
        parsed.command = SlashCommand::Usage;
    } else if (lower == "/insights") {
        // Token 账本单 A5:/insights 只认词,二级参数(--since/--sessions/
        // --all-workspaces/status/clean)在 ParseInsightsCommand
        // (app/commands/insights_commands)拆。
        parsed.command = SlashCommand::Insights;
    } else if (lower == "/compact") {
        parsed.command = SlashCommand::Compact;
    } else if (lower == "/think" || lower == "/effort") {
        // M10:/effort 是 /think 的别名,同一个命令枚举值——解析层面这俩
        // 词天生等价,不用另开一个 SlashCommand::Effort 分叉出两套处理逻辑。
        parsed.command = SlashCommand::Think;
    } else if (lower == "/skills") {
        parsed.command = SlashCommand::Skills;
    } else if (lower == "/skill") {
        parsed.command = SlashCommand::Skill;
    } else if (lower == "/mcp") {
        parsed.command = SlashCommand::Mcp;
    } else if (lower == "/lsp") {
        parsed.command = SlashCommand::Lsp;
    } else if (lower == "/todos") {
        parsed.command = SlashCommand::Todos;
    } else if (lower == "/plugins") {
        parsed.command = SlashCommand::Plugins;
    } else if (lower == "/plugin") {
        parsed.command = SlashCommand::Plugin;
    } else if (lower == "/hooks") {
        parsed.command = SlashCommand::Hooks;
    } else if (lower == "/tools") {
        parsed.command = SlashCommand::Tools;
    } else if (lower == "/memory") {
        parsed.command = SlashCommand::Memory;
    } else if (lower == "/sessions") {
        parsed.command = SlashCommand::Sessions;
    } else if (lower == "/resume") {
        parsed.command = SlashCommand::Resume;
    } else if (lower == "/archive") {
        parsed.command = SlashCommand::Archive;
    } else if (lower == "/delete") {
        parsed.command = SlashCommand::Delete;
    } else if (lower == "/export") {
        parsed.command = SlashCommand::Export;
    } else if (lower == "/copy") {
        parsed.command = SlashCommand::Copy;
    } else if (lower == "/title") {
        parsed.command = SlashCommand::Title;
    } else if (lower == "/soul") {
        parsed.command = SlashCommand::Soul;
    } else if (lower == "/prompt") {
        parsed.command = SlashCommand::Prompt;
    } else if (lower == "/language" || lower == "/lang") {
        // /lang 是 /language 的省事别名,跟 LUBANCODE_LANG 环境变量对得上口。
        parsed.command = SlashCommand::Language;
    } else if (lower == "/image") {
        parsed.command = SlashCommand::Image;
    } else if (lower == "/worktree") {
        parsed.command = SlashCommand::Worktree;
    } else if (lower == "/background" || lower == "/bg") {
        // /bg 是省事别名,跟 /background 同义。
        parsed.command = SlashCommand::Background;
    } else if (lower == "/record") {
        parsed.command = SlashCommand::Record;
    } else if (lower == "/peers") {
        parsed.command = SlashCommand::Peers;
    } else if (lower == "/send") {
        parsed.command = SlashCommand::Send;
    } else if (lower == "/peerperm") {
        parsed.command = SlashCommand::Peerperm;
    } else if (lower == "/doctor") {
        parsed.command = SlashCommand::Doctor;
    } else if (lower == "/telemetry") {
        // 端云协同可观测单 T1:只读状态面(裸敲 = status)。
        parsed.command = SlashCommand::Telemetry;
    } else if (lower == "/keymap") {
        parsed.command = SlashCommand::Keymap;
    } else if (lower == "/trace") {
        parsed.command = SlashCommand::Trace;
    } else if (lower == "/goal") {
        // 持久目标单:/goal 是正门(objective/status/edit/pause/resume/clear
        // 的二级解析在 ParseGoalCommand,这里只认词)。
        parsed.command = SlashCommand::Goal;
    } else if (lower == "/loop") {
        // loop 单:/loop 是正门([interval] [prompt] 与
        // list/status/pause/resume/stop/run 的二级解析在 ParseLoopCommand)。
        parsed.command = SlashCommand::Loop;
    } else if (lower == "/plan") {
        // Plan 模式单:/plan 是正门(裸敲/status/off/review/带正文)。
        parsed.command = SlashCommand::Plan;
    } else if (lower == "/package") {
        // 统一 Package 封装单阶段 1:/package 只读面(list/show/doctor,
        // 二级解析在 ParsePackageCommand,这里只认词)。
        parsed.command = SlashCommand::Package;
    } else if (lower == "/evolve") {
        // 自进化闭环阶段 1:/evolve 只读面(status/list/show,二级解析在
        // ParseEvolveCommand,这里只认词)。
        parsed.command = SlashCommand::Evolve;
    } else if (lower == "/workflow") {
        // Workflows 自然语言编排单:/workflow 是正门(list/show/graph/
        // validate/run/...),子命令解析在 workflow 层,这里只认词。
        parsed.command = SlashCommand::Workflow;
    } else if (lower == "/agents") {
        // 自定义 Agent 单阶段 1:/agents 只列 Agent Catalog(只读)。
        parsed.command = SlashCommand::Agents;
    } else if (lower == "/agent") {
        // 自定义 Agent 单阶段 1:/agent 的子命令(doctor/...)在
        // app/commands/agent_commands 拆,这里只认词。
        parsed.command = SlashCommand::Agent;
    } else if (lower == "/instructions") {
        // AGENTS.md 作用域单 P1:/instructions 是正门(裸敲/path <路径>/
        // reload 的二级解析在 ParseInstructionsCommand,这里只认词)。
        parsed.command = SlashCommand::Instructions;
    } else if (lower == "/channels") {
        // 多渠道消息接入单阶段 2:/channels 只读列渠道账号(配置 × 运行态)。
        parsed.command = SlashCommand::Channels;
    } else if (lower == "/channel") {
        // 多渠道消息接入单阶段 2:/channel 的子命令(show/doctor/start/
        // stop/restart)在 app/commands/channel_commands 拆,这里只认词。
        parsed.command = SlashCommand::Channel;
    } else {
        // 不认得的 / 词:仍是 Unknown(语义不变),但把剥掉 / 的原词记在
        // alias_word 里——会话层对 Unknown 先查 WorkflowCatalog,查着了
        // 走 /<alias> 直呼,查不着照旧打"XXX 不认得"。内建词永远居首,
        // 这条路只兜"不认得"的尾巴(单子"Slash alias 与冲突规矩")。
        parsed.command = SlashCommand::Unknown;
        parsed.alias_word = word.substr(1);
    }
    return parsed;
}

ParsedProviderCommand ParseProviderCommand(const std::string& args) {
    ParsedProviderCommand parsed;
    std::istringstream input(args);
    std::vector<std::string> words;
    for (std::string word; input >> word;) {
        words.push_back(std::move(word));
    }

    // 裸敲 /provider 与 /provider list 同义，方便查当前端。
    if (words.empty() || (words.size() == 1 && ToLower(words[0]) == "list")) {
        parsed.action = ProviderCommandAction::List;
        return parsed;
    }

    const std::string action = ToLower(words[0]);
    parsed.bad_word = words[0];  // Invalid 时给容错层看的第一词原始拼写
    if (action == "edit") {
        // 容错单:/provider edit <名字> 进向导改旧 provider。裸敲 edit(TTY)
        // 开选择列表;词数超了(3 个及以上)照旧 Invalid,提示走容错短用法。
        if (words.size() == 1) {
            parsed.action = ProviderCommandAction::EditInteractive;
        } else if (words.size() == 2) {
            parsed.action = ProviderCommandAction::Edit;
            parsed.name = words[1];
        }
        return parsed;
    }
    if (action == "refresh") {
        if (words.size() == 1) parsed.action = ProviderCommandAction::Refresh;
        return parsed;
    }
    if (action == "switch") {
        if (words.size() == 2 || words.size() == 3) {
            parsed.action = ProviderCommandAction::Switch;
            parsed.name = words[1];
            if (words.size() == 3) {
                parsed.model = words[2];
            }
            return parsed;
        }
        if (words.size() == 1) {
            // 裸敲 /provider switch:意图很明白(要换一家),不判 Invalid、不倒
            // 总帮助。TTY 下开选择器,非 TTY 给 switch 专用短用法。
            parsed.action = ProviderCommandAction::SwitchInteractive;
        }
        return parsed;
    }
    if (action == "remove") {
        if (words.size() == 2) {
            parsed.action = ProviderCommandAction::Remove;
            parsed.name = words[1];
        }
        return parsed;
    }
    if (action == "set") {
        // /provider set <名字> <字段> <值...>。字段名不认得也照单全收(留给
        // main.cpp 报"不认得的字段"这种更具体的错误),这里只管拆词——但
        // extra_body/extra_header 这两个字段的"值"不能按空格切词(一坨
        // JSON、或者带空格的头值),得从原始 args 里按位置抠剩下的原文,
        // 所以这里不能只用前面 istringstream 切好的 words,得重新按位置扫
        // 一遍 args。
        std::size_t pos = 0;
        NextToken(args, pos);  // "set" 本身(words[0]),这里只是把 pos 往前挪
        const auto name_tok = NextToken(args, pos);
        const auto field_tok = NextToken(args, pos);
        if (!name_tok.has_value() || !field_tok.has_value()) {
            return parsed;
        }
        const std::string field_lower = ToLower(*field_tok);

        if (field_lower == "extra_body") {
            parsed.action = ProviderCommandAction::Set;
            parsed.name = *name_tok;
            parsed.field = field_lower;
            parsed.value = Trim(args.substr(pos));
            return parsed;
        }
        if (field_lower == "extra_header") {
            // header 名字保留原始大小写——HTTP 头名字大小写是用户自己敲的
            // 原样,这一层不该悄悄改掉。
            const auto header_tok = NextToken(args, pos);
            if (!header_tok.has_value()) {
                return parsed;  // 缺 header 名字,词数不够,Invalid
            }
            parsed.action = ProviderCommandAction::Set;
            parsed.name = *name_tok;
            parsed.field = field_lower;
            parsed.header_name = *header_tok;
            parsed.value = Trim(args.substr(pos));
            return parsed;
        }

        // 老字段(目前只有 native_web_search):固定四个词,字段名和值都
        // 小写化,第四个词之后不许再冒出别的词——跟改造前逐字一样,不能
        // 破坏既有单测。
        const auto value_tok = NextToken(args, pos);
        if (!value_tok.has_value()) {
            return parsed;
        }
        std::size_t trailing_pos = pos;
        if (NextToken(args, trailing_pos).has_value()) {
            return parsed;  // 冒出第五个词,词数超了
        }
        parsed.action = ProviderCommandAction::Set;
        parsed.name = *name_tok;
        parsed.field = field_lower;
        parsed.value = ToLower(*value_tok);
        return parsed;
    }
    if (action != "add") {
        return parsed;
    }

    // 裸敲 `/provider add`,或者只给了名字 `/provider add 名字`:进分步向导
    // (main.cpp 走 RunProviderAddWizard)。三个词及以上但凑不满一行式必填
    // 的 <名字> <base_url> <协议> 三项(words.size() 3),不当向导触发——
    // 那是用户敲一行式敲到一半、漏了参数,维持原样 Invalid,提示用法。
    if (words.size() <= 2) {
        parsed.action = ProviderCommandAction::Add;
        parsed.wizard = true;
        if (words.size() == 2) {
            parsed.name = words[1];
        }
        return parsed;
    }
    if (words.size() < 4) {
        return parsed;
    }

    parsed.action = ProviderCommandAction::Add;
    parsed.name = words[1];
    parsed.base_url = words[2];
    parsed.wire = ToLower(words[3]);
    bool saw_key_env = false;
    bool saw_key = false;
    bool saw_model = false;
    bool saw_effort = false;
    bool saw_window = false;
    for (std::size_t i = 4; i < words.size();) {
        if (i + 1 >= words.size()) {
            parsed.action = ProviderCommandAction::Invalid;
            return parsed;
        }
        const std::string& option = words[i];
        const std::string& value = words[i + 1];
        if (option == "--key-env" && !saw_key_env) {
            parsed.key_env = value;
            saw_key_env = true;
        } else if (option == "--key" && !saw_key) {
            parsed.key = value;
            saw_key = true;
        } else if (option == "--model" && !saw_model) {
            parsed.model = value;
            saw_model = true;
        } else if (option == "--effort" && !saw_effort) {
            parsed.effort = value;
            saw_effort = true;
        } else if (option == "--window" && !saw_window) {
            parsed.window = value;
            saw_window = true;
        } else {
            parsed.action = ProviderCommandAction::Invalid;
            return parsed;
        }
        i += 2;
    }
    return parsed;
}

bool CanRemoveProvider(const std::string& active_provider, const std::string& name) {
    return active_provider != name;
}

// ---------------------------------------------------------------------------
// /provider 子命令容错(容错单)
// ---------------------------------------------------------------------------

std::vector<std::string> ProviderSubcommands() {
    return {"list", "refresh", "add", "switch", "remove", "set", "edit"};
}

namespace {

// 经典 Levenshtein(两行滚动数组)。子命令最长 7 个字符,直接算全表,不做
// 提前剪枝——量太小,剪枝反倒是噪音。
std::size_t EditDistanceAscii(std::string_view a, std::string_view b) {
    const std::size_t rows = a.size() + 1;
    const std::size_t cols = b.size() + 1;
    std::vector<std::size_t> prev(cols);
    std::vector<std::size_t> curr(cols);
    for (std::size_t j = 0; j < cols; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i < rows; ++i) {
        curr[0] = i;
        for (std::size_t j = 1; j < cols; ++j) {
            const std::size_t substitution = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, substitution});
        }
        prev.swap(curr);
    }
    return prev[cols - 1];
}

}  // namespace

std::optional<std::string> NearestProviderSubcommand(const std::string& word) {
    // 空词/离谱长词不比——它们跟谁都不沾亲。
    if (word.empty() || word.size() > 16) {
        return std::nullopt;
    }
    const std::string lowered = ToLower(word);
    constexpr std::size_t kMaxDistance = 2;
    std::optional<std::string> best;
    std::size_t best_distance = kMaxDistance + 1;
    for (const std::string& candidate : ProviderSubcommands()) {
        // 长度差超过阈值的不用进算法,省一趟 DP。
        if (lowered.size() > candidate.size() + kMaxDistance ||
            candidate.size() > lowered.size() + kMaxDistance) {
            continue;
        }
        const std::size_t distance = EditDistanceAscii(lowered, candidate);
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
        // 距离相同不换人:清单序即推荐序,排前面的赢(纯词如 "a" 同时离
        // add/set 两边 2 步,取 add)。
    }
    if (best_distance > kMaxDistance) {
        return std::nullopt;
    }
    return best;
}

std::string ProviderSubcommandUsageLine(const std::string& subcommand) {
    const std::vector<std::string> known = ProviderSubcommands();
    if (std::find(known.begin(), known.end(), subcommand) == known.end()) {
        return std::string();  // 认不得的子命令不编用法,tr 缺键会回 key 本身
    }
    if (subcommand == "switch") {
        return tr("cmd.provider.switch.usage_short");  // 既有键,不另立门户
    }
    return tr("cmd.provider.usage_short." + subcommand);
}

ParsedRecordCommand ParseRecordCommand(const std::string& args) {
    ParsedRecordCommand parsed;
    std::size_t pos = 0;
    const auto action = NextToken(args, pos);
    if (!action.has_value()) {
        parsed.action = RecordCommandAction::Status;  // 裸敲 /record 看状态
        return parsed;
    }
    const std::string verb = ToLower(*action);

    const auto rest = [&]() { return Trim(args.substr(pos)); };

    if (verb == "status") {
        parsed.action = RecordCommandAction::Status;
        return parsed;
    }
    if (verb == "start") {
        const auto name = NextToken(args, pos);
        if (!name.has_value() || NextToken(args, pos).has_value()) {
            return parsed;  // 没名字/名字多于一个词,Invalid
        }
        parsed.action = RecordCommandAction::Start;
        parsed.name = *name;
        return parsed;
    }
    if (verb == "note") {
        // 备注原文整段保留(可以有空格);没正文就 Invalid。
        const std::string text = rest();
        if (!text.empty()) {
            parsed.action = RecordCommandAction::Note;
            parsed.text = text;
        }
        return parsed;
    }
    if (verb == "pause") {
        parsed.action = RecordCommandAction::Pause;
        return parsed;
    }
    if (verb == "resume") {
        parsed.action = RecordCommandAction::Resume;
        return parsed;
    }
    if (verb == "stop") {
        parsed.action = RecordCommandAction::Stop;
        return parsed;
    }
    if (verb == "cancel") {
        parsed.action = RecordCommandAction::Cancel;
        return parsed;
    }
    if (verb == "list") {
        parsed.action = RecordCommandAction::List;
        return parsed;
    }
    if (verb == "discard") {
        const auto id = NextToken(args, pos);
        if (id.has_value() && !NextToken(args, pos).has_value()) {
            parsed.action = RecordCommandAction::Discard;
            parsed.name = *id;
        }
        return parsed;
    }
    if (verb == "install") {
        const auto id = NextToken(args, pos);
        if (!id.has_value()) {
            return parsed;
        }
        parsed.action = RecordCommandAction::Install;
        parsed.name = *id;
        if (const auto where = NextToken(args, pos); where.has_value()) {
            const std::string level = ToLower(*where);
            if (level == "project" || level == "p") {
                parsed.to_project = true;
            } else if (level == "home" || level == "h") {
                parsed.to_project = false;
            } else {
                parsed.action = RecordCommandAction::Invalid;  // 认不得的层级词
                return parsed;
            }
            if (NextToken(args, pos).has_value()) {
                parsed.action = RecordCommandAction::Invalid;  // 冒出第四个词
            }
        }
        return parsed;
    }
    return parsed;  // 认不得的动作,保持 Invalid
}

ParsedInstructionsCommand ParseInstructionsCommand(const std::string& args) {
    ParsedInstructionsCommand parsed;
    const std::string trimmed = Trim(args);

    // 裸敲 /instructions:当前 cwd 的基线链。
    if (trimmed.empty()) {
        parsed.action = InstructionsCommandAction::Baseline;
        return parsed;
    }

    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string first = (space == std::string::npos) ? trimmed : trimmed.substr(0, space);
    const std::string rest = (space == std::string::npos) ? std::string() : Trim(trimmed.substr(space + 1));
    const std::string lower = ToLower(first);
    parsed.bad_word = first;

    if (lower == "reload") {
        // reload 无参;带尾巴按 Invalid(同 /goal status 的取舍)。
        if (!rest.empty()) {
            parsed.action = InstructionsCommandAction::Invalid;
            return parsed;
        }
        parsed.action = InstructionsCommandAction::Reload;
        return parsed;
    }
    if (lower == "path") {
        // 目标路径原样递给 Resolver(相对/绝对、文件/目录都认),这里只
        // 要求非空。
        if (rest.empty()) {
            parsed.action = InstructionsCommandAction::Invalid;
            return parsed;
        }
        parsed.action = InstructionsCommandAction::Path;
        parsed.target = rest;
        return parsed;
    }
    parsed.action = InstructionsCommandAction::Invalid;
    return parsed;
}

ParsedGoalCommand ParseGoalCommand(const std::string& args) {
    ParsedGoalCommand parsed;
    const std::string trimmed = Trim(args);
    // 裸敲 /goal:看账,不发模型。
    if (trimmed.empty()) {
        parsed.action = GoalCommandAction::View;
        return parsed;
    }

    // `--` 消歧:"/goal -- pause all jobs" 里 pause 是正文不是子命令。
    if (trimmed == "--" || trimmed.rfind("-- ", 0) == 0) {
        parsed.dashdash = true;
        parsed.objective = Trim(trimmed.substr(2));
        if (parsed.objective.empty()) {
            parsed.action = GoalCommandAction::Invalid;
            parsed.bad_word = "--";
            return parsed;
        }
        parsed.action = GoalCommandAction::Create;
        return parsed;
    }

    // 第一个词按空白切。
    const std::size_t space = trimmed.find_first_of(" 	");
    const std::string first = (space == std::string::npos) ? trimmed : trimmed.substr(0, space);
    const std::string rest = (space == std::string::npos) ? std::string() : Trim(trimmed.substr(space + 1));
    const std::string lower = ToLower(first);
    parsed.bad_word = first;

    if (lower == "status") {
        // status 无参;带尾巴按 Invalid(避免 "status of migration" 的正文
        // 歧义——要写正文用 `--`)。
        if (!rest.empty()) {
            parsed.action = GoalCommandAction::Invalid;
            return parsed;
        }
        parsed.action = GoalCommandAction::Status;
        return parsed;
    }
    if (lower == "pause") {
        parsed.action = GoalCommandAction::Pause;
        return parsed;
    }
    if (lower == "resume") {
        parsed.action = GoalCommandAction::Resume;
        return parsed;
    }
    if (lower == "clear") {
        parsed.action = GoalCommandAction::Clear;
        return parsed;
    }
    if (lower == "edit") {
        // /goal edit -- <text>:-- 后全算正文;不带 -- 时正文若恰以 -- 起
        // (用户想写 "--fix" 这类)不误吞。
        std::string body = rest;
        bool dd = false;
        if (body == "--" || body.rfind("-- ", 0) == 0) {
            dd = true;
            body = Trim(body.substr(2));
        }
        if (body.empty()) {
            parsed.action = GoalCommandAction::Invalid;
            return parsed;
        }
        parsed.action = GoalCommandAction::Edit;
        parsed.objective = body;
        parsed.dashdash = dd;
        return parsed;
    }
    // 其余一切:objective 正文(含以 pause/edit 开头的正文——那是要靠 `--`
    // 之外的普通目标文本,原样收)。
    parsed.action = GoalCommandAction::Create;
    parsed.objective = trimmed;
    return parsed;
}

namespace {
// token 是不是合法 interval 形状(正整数 + m/h/d，单位后到头)。
// 与 runtime::loop::ParseLoopInterval 同规矩，但不引 runtime 头——cli 层
// 零实现依赖，数值校验(1m..7d)这里先验一道，会话层拿
// ParseLoopInterval 再验第二道。
bool LooksLikeLoopIntervalToken(const std::string& token) {
    if (token.size() < 2) {
        return false;
    }
    std::size_t i = 0;
    while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
        ++i;
    }
    if (i == 0 || i + 1 != token.size()) {
        return false;
    }
    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    if (unit != 'm' && unit != 'h' && unit != 'd') {
        return false;
    }
    const std::string digits = token.substr(0, token.size() - 1);
    if (digits.size() > 9) {
        return false;
    }
    const long long value = std::strtoll(digits.c_str(), nullptr, 10);
    if (value <= 0) {
        return false;
    }
    long long seconds = value;
    if (unit == 'm') {
        seconds = value * 60;
    } else if (unit == 'h') {
        seconds = value * 3600;
    } else {
        seconds = value * 86400;
    }
    return seconds >= 60 && seconds <= 604800;  // 1m..7d
}

// token 是不是“长得像 interval 但不合法”(数字起头、后面跟字母):
// 0m/8d/1h30m/30s/超大数——这些明报，不静默当 prompt。
bool LooksLikeBrokenInterval(const std::string& token) {
    // 负数起头(-5m):第二位是数字就算坏 interval(否则可能是普通 "--" 开头的正文,那条路已在前面收过)。
    if (token.size() >= 3 && token[0] == '-' && token[1] >= '0' && token[1] <= '9') {
        return true;
    }
    if (token.empty() || !(token[0] >= '0' && token[0] <= '9')) {
        return false;
    }
    // 数字后跟字母或小数点(0m/8d/1h30m/30s/1.5h/超大数):
    // 这些长得像 interval 但不合法,明报不静默当 prompt。
    const std::size_t digits = token.find_first_not_of("0123456789");
    if (digits == std::string::npos || digits == 0) {
        return false;
    }
    // 数字后恰一个字母且不是 m/h/d(30s/5x):明报。
    // 数字后多个字母(5migrate/5unix):是普通词,不咬。
    // 数字后非字母非数字(1.5h/1h30m 的那个点/字母串):明报。
    const unsigned char c = static_cast<unsigned char>(token[digits]);
    if (std::isalpha(c) != 0) {
        if (token.size() == digits + 1) {
            const char unit = static_cast<char>(std::tolower(c));
            if (unit != 'm' && unit != 'h' && unit != 'd') {
                return true;  // 30s/5x:单位不认
            }
            // 单位是 m/h/d 但数值越界(0m/8d/超大数):明报,
            // 不静默当 prompt。
            if (digits > 9) {
                return true;  // 溢出前先拒
            }
            const long long v = std::strtoll(token.substr(0, digits).c_str(), nullptr, 10);
            long long secs = v;
            if (unit == 'm') {
                secs = v * 60;
            } else if (unit == 'h') {
                secs = v * 3600;
            } else {
                secs = v * 86400;
            }
            return v <= 0 || secs < 60 || secs > 604800;
        }
        // 多个字母:再看后面有没有数字(1h30m 连写报坏;
        // 5migrate 这类普通词不咬)。
        const std::size_t more =
            token.find_first_of("0123456789", digits + 1);
        return more != std::string::npos;
    }
    return c == '.';  // 1.5h
}
}  // namespace

ParsedLoopCommand ParseLoopCommand(const std::string& args) {
    ParsedLoopCommand parsed;
    const std::string trimmed = Trim(args);

    // 裸敲 /loop:默认 10m + loop.md/内置 prompt(裸敲的"创建"语义优先于
    // "列状态";查询统一走 list)。
    if (trimmed.empty()) {
        parsed.action = LoopCommandAction::Create;
        return parsed;
    }

    // `--` 消歧:后面全算 prompt。
    if (trimmed == "--" || trimmed.rfind("-- ", 0) == 0) {
        parsed.dashdash = true;
        parsed.prompt = Trim(trimmed.substr(2));
        if (parsed.prompt.empty()) {
            parsed.action = LoopCommandAction::Invalid;
            parsed.bad_word = "--";
            parsed.error_hint = "-- 后面要有正文";
            return parsed;
        }
        parsed.action = LoopCommandAction::Create;
        return parsed;
    }

    std::size_t pos = 0;
    const auto first = NextToken(args, pos);
    if (!first.has_value()) {
        parsed.action = LoopCommandAction::Create;
        return parsed;
    }
    const std::string lower = ToLower(*first);
    parsed.bad_word = *first;
    const std::string rest = Trim(args.substr(pos));

    // 子命令:严格等于这六个词(status/pause/resume/stop/run 需带目标)。
    if (lower == "list") {
        if (!rest.empty()) {
            parsed.error_hint = "list 不带参数";
            return parsed;
        }
        parsed.action = LoopCommandAction::List;
        return parsed;
    }
    if (lower == "status" || lower == "pause" || lower == "resume" || lower == "stop" ||
        lower == "run") {
        if (rest.empty()) {
            parsed.error_hint = lower + " 要带 task id 或 all";
            return parsed;
        }
        // 目标只收一个词(全 id/裸数字/all);多词当 Invalid(防把 prompt
        // 误吞成 id)。
        std::size_t pos2 = 0;
        const auto target = NextToken(rest, pos2);
        if (!target.has_value() || Trim(rest.substr(pos2)) != "") {
            parsed.error_hint = "目标只收一个 task id 或 all";
            return parsed;
        }
        parsed.task_ref = *target;
        parsed.action = lower == "status" ? LoopCommandAction::Status
                        : lower == "pause" ? LoopCommandAction::Pause
                        : lower == "resume" ? LoopCommandAction::Resume
                        : lower == "stop"  ? LoopCommandAction::Stop
                                            : LoopCommandAction::Run;
        return parsed;
    }

    // interval 形状:LooksLike 且能解出合法值才当间隔;歪形状给 Invalid
    //(用户想写 interval 但写错了,别静默当 prompt)。
    if (LooksLikeLoopIntervalToken(*first)) {
        parsed.interval_text = *first;
        if (!rest.empty() && rest.rfind("-- ", 0) == 0) {
            parsed.dashdash = true;
            parsed.prompt = Trim(rest.substr(3));
        } else if (rest == "--") {
            parsed.error_hint = "-- 后面要有正文";
            return parsed;
        } else {
            parsed.prompt = rest;
        }
        parsed.action = LoopCommandAction::Create;
        return parsed;
    }
    // 数字+错单位/超限这类"像 interval 但不合法"的,明报不静默。
    if (LooksLikeBrokenInterval(*first)) {
        parsed.error_hint = "interval 只认 <正整数>m|h|d,最小 1m,最大 7d";
        return parsed;
    }

    // 其余一切:默认间隔的 prompt 正文。
    parsed.prompt = trimmed;
    parsed.action = LoopCommandAction::Create;
    return parsed;
}

ParsedPlanCommand ParsePlanCommand(const std::string& args) {
    ParsedPlanCommand parsed;
    const std::string trimmed = Trim(args);
    if (trimmed.empty()) {
        parsed.action = PlanCommandAction::Enter;
        return parsed;
    }
    // 第一个词是子命令就分路;认不得的子词按"带正文的规划请求"处理——
    // "/plan 帮我设计缓存层"的正文首个词不是四枚子词,照 EnterWithTask。
    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string first = ToLower(space == std::string::npos ? trimmed : trimmed.substr(0, space));
    if (space == std::string::npos) {
        // 单词:只可能是子命令(带正文至少俩词,正文本身一个词的场合见下)。
        if (first == "status" || first == "off" || first == "review") {
            parsed.action = first == "status"  ? PlanCommandAction::Status
                            : first == "off"   ? PlanCommandAction::Off
                                                 : PlanCommandAction::Review;
            return parsed;
        }
        // 整个 args 是一个词但不是子词:当规划请求正文(如 "/plan 查登录死锁")。
        parsed.action = PlanCommandAction::EnterWithTask;
        parsed.description = trimmed;
        return parsed;
    }
    if (first == "status" || first == "off" || first == "review") {
        // 子词后面还有词:usage 说不过去,Invalid(不悄悄把 "status foo" 当正文)。
        return parsed;
    }
    parsed.action = PlanCommandAction::EnterWithTask;
    parsed.description = trimmed;
    return parsed;
}

const std::vector<SlashCommandInfo>& AllSlashCommands() {
    // i18n:说明文字按当前语言现查(tr),语言切换后惰性重建——静态表 +
    // 记住"上次是按哪种语言建的",不一致就重来一遍。交互循环是单线程消费
    // (Tab 补全、/help),不用加锁。
    static std::vector<SlashCommandInfo> commands;
    static std::string built_for;
    if (commands.empty() || built_for != CurrentLanguage()) {
        built_for = CurrentLanguage();
        commands = {
            {"/help", tr("slash.desc.help")},
            {"/model", tr("slash.desc.model")},
            {"/provider", tr("slash.desc.provider")},
            {"/worktree", tr("slash.desc.worktree")},
            {"/config", tr("slash.desc.config")},
            {"/update", tr("slash.desc.update")},
            {"/init", tr("slash.desc.init")},
            {"/instructions", tr("slash.desc.instructions")},
            {"/language", tr("slash.desc.language")},
            {"/image", tr("slash.desc.image")},
            {"/clear", tr("slash.desc.clear")},
            {"/exit", tr("slash.desc.exit")},
            {"/context", tr("slash.desc.context")},
            {"/usage", tr("slash.desc.usage")},
            {"/insights", tr("slash.desc.insights")},
            {"/compact", tr("slash.desc.compact")},
            {"/think", tr("slash.desc.think")},
            {"/effort", tr("slash.desc.effort")},
            {"/skills", tr("slash.desc.skills")},
            {"/skill", tr("slash.desc.skill")},
            {"/mcp", tr("slash.desc.mcp")},
            {"/lsp", tr("slash.desc.lsp")},
            {"/todos", tr("slash.desc.todos")},
            {"/plugins", tr("slash.desc.plugins")},
            {"/plugin", tr("slash.desc.plugin")},
            {"/agents", tr("slash.desc.agents")},
            {"/agent", tr("slash.desc.agent")},
            {"/tools", tr("slash.desc.tools")},
            {"/memory", tr("slash.desc.memory")},
            {"/sessions", tr("slash.desc.sessions")},
            {"/resume", tr("slash.desc.resume")},
            {"/archive", tr("slash.desc.archive")},
            {"/delete", tr("slash.desc.delete")},
            {"/export", tr("slash.desc.export")},
            {"/copy", tr("slash.desc.copy")},
            {"/title", tr("slash.desc.title")},
            {"/soul", tr("slash.desc.soul")},
            {"/prompt", tr("slash.desc.prompt")},
            {"/background", tr("slash.desc.background")},
            {"/record", tr("slash.desc.record")},
            {"/peers", tr("slash.desc.peers")},
            {"/send", tr("slash.desc.send")},
            {"/peerperm", tr("slash.desc.peerperm")},
            {"/doctor", tr("slash.desc.doctor")},
            {"/keymap", tr("slash.desc.keymap")},
            {"/workflow", tr("slash.desc.workflow")},
            {"/goal", tr("slash.desc.goal")},
            {"/loop", tr("slash.desc.loop")},
            {"/plan", tr("slash.desc.plan")},
            {"/package", tr("slash.desc.package")},
            {"/evolve", tr("slash.desc.evolve")},
            {"/channels", tr("slash.desc.channels")},
            {"/channel", tr("slash.desc.channel")},
            {"/telemetry", tr("slash.desc.telemetry")},
        };
    }
    return commands;
}

std::vector<std::string> FormatSlashCommandListLines() {
    // 帮助清单的唯一排版口(P3-2):--help 的斜杠命令节与 /help 的正文都打
    // 这里出的行,与 Tab 补全同一份 AllSlashCommands,名单不许各列各的。
    const std::vector<SlashCommandInfo>& commands = AllSlashCommands();
    std::size_t name_width = 0;
    for (const SlashCommandInfo& command : commands) {
        name_width = name_width > command.name.size() ? name_width : command.name.size();
    }
    std::vector<std::string> lines;
    lines.reserve(commands.size());
    for (const SlashCommandInfo& command : commands) {
        std::string line = "  ";
        line += command.name;
        line.append(name_width - command.name.size() + 2, ' ');
        line += command.description;
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace lubancode::cli
