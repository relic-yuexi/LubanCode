#include "cli/slash_commands.hpp"

#include <algorithm>
#include <cctype>
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
    } else if (lower == "/export") {
        parsed.command = SlashCommand::Export;
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
    } else {
        parsed.command = SlashCommand::Unknown;
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
            {"/language", tr("slash.desc.language")},
            {"/image", tr("slash.desc.image")},
            {"/clear", tr("slash.desc.clear")},
            {"/exit", tr("slash.desc.exit")},
            {"/context", tr("slash.desc.context")},
            {"/compact", tr("slash.desc.compact")},
            {"/think", tr("slash.desc.think")},
            {"/effort", tr("slash.desc.effort")},
            {"/skills", tr("slash.desc.skills")},
            {"/skill", tr("slash.desc.skill")},
            {"/mcp", tr("slash.desc.mcp")},
            {"/lsp", tr("slash.desc.lsp")},
            {"/todos", tr("slash.desc.todos")},
            {"/plugins", tr("slash.desc.plugins")},
            {"/tools", tr("slash.desc.tools")},
            {"/memory", tr("slash.desc.memory")},
            {"/sessions", tr("slash.desc.sessions")},
            {"/resume", tr("slash.desc.resume")},
            {"/export", tr("slash.desc.export")},
            {"/title", tr("slash.desc.title")},
            {"/soul", tr("slash.desc.soul")},
            {"/prompt", tr("slash.desc.prompt")},
            {"/background", tr("slash.desc.background")},
            {"/record", tr("slash.desc.record")},
            {"/peers", tr("slash.desc.peers")},
            {"/send", tr("slash.desc.send")},
            {"/peerperm", tr("slash.desc.peerperm")},
            {"/doctor", tr("slash.desc.doctor")},
        };
    }
    return commands;
}

}  // namespace lubancode::cli
