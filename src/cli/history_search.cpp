// history_search.hpp 的实现。

#include "cli/history_search.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "cli/i18n.hpp"
#include "cli/keymap.hpp"  // ChordFor/FormatKeyChord(首行键名反查)
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth / DisplayWidthUtf8

namespace lubancode::cli {

namespace {

// ASCII 折小写(中文等多字节原样,按字节比较安全——UTF-8 后续字节不带
// 大小写语义)。
std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 多行提问的展示首行(截前 60 码点,超长补 …;换行换成 ⏎ 保信息)。
std::string FirstDisplayLine(const std::string& text) {
    std::size_t chars = 0;
    std::string out;
    for (std::size_t i = 0; i < text.size() && chars < 60;) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            bytes = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            bytes = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            bytes = 4;
        }
        bytes = (std::min)(bytes, text.size() - i);
        if (text[i] == '\n') {
            out += "⏎";
            ++chars;
            ++i;
            continue;
        }
        out += text.substr(i, bytes);
        ++chars;
        i += bytes;
    }
    if (chars == 60 && text.size() > out.size()) {
        out += "…";
    }
    return out;
}

std::string ProjectShortName(const std::string& key) {
    if (key.empty()) {
        return {};
    }
    const std::size_t slash = key.find_last_of('/');
    return slash == std::string::npos ? key : key.substr(slash + 1);
}

}  // namespace

HistorySearchScope NextHistorySearchScope(HistorySearchScope scope) {
    switch (scope) {
        case HistorySearchScope::Session: return HistorySearchScope::Project;
        case HistorySearchScope::Project: return HistorySearchScope::All;
        case HistorySearchScope::All: return HistorySearchScope::Session;
    }
    return HistorySearchScope::Session;
}

std::vector<PromptHistoryEntry> BuildHistorySearchIndex(const PromptHistoryDataset& dataset,
                                                        HistorySearchScope scope, std::size_t max) {
    std::vector<PromptHistoryEntry> filtered;
    filtered.reserve(dataset.entries.size());
    for (const auto& entry : dataset.entries) {
        if (entry.text.empty()) {
            continue;
        }
        switch (scope) {
            case HistorySearchScope::Session:
                if (!dataset.current_session_id.empty() && entry.session_id == dataset.current_session_id) {
                    filtered.push_back(entry);
                }
                break;
            case HistorySearchScope::Project:
                if (!dataset.current_project_key.empty() && entry.project_key == dataset.current_project_key) {
                    filtered.push_back(entry);
                }
                break;
            case HistorySearchScope::All:
                filtered.push_back(entry);
                break;
        }
    }
    // 事件身份去重:同一事件(session id + 场内序号)只留一条——存档侧与
    // 活历史侧认出同一事件即合并,先见的(数据集里排前的存档份,ts 是档上
    // 真时间)为准;同文不同事件(用户真发了两次)身份不同,各自保留。
    std::vector<PromptHistoryEntry> deduped;
    deduped.reserve(filtered.size());
    std::set<std::pair<std::string, std::size_t>> seen_ids;
    for (const auto& entry : filtered) {  // 旧→新
        if (!seen_ids.insert(std::make_pair(entry.session_id, entry.event_seq)).second) {
            continue;
        }
        deduped.push_back(entry);
    }
    std::reverse(deduped.begin(), deduped.end());  // 输出新→旧
    if (deduped.size() > max) {
        deduped.resize(max);
    }
    return deduped;
}

std::vector<std::size_t> SearchHistoryEntries(const std::vector<PromptHistoryEntry>& index,
                                              const std::string& query, std::size_t limit) {
    std::vector<std::size_t> out;
    if (index.empty()) {
        return out;
    }
    const std::string needle = ToLowerAscii(query);
    for (std::size_t i = 0; i < index.size() && out.size() < limit; ++i) {  // index 新→旧
        if (needle.empty() ||
            ToLowerAscii(index[i].text).find(needle) != std::string::npos) {
            out.push_back(i);
        }
    }
    return out;
}

void HistorySearchSession::Open(PromptHistoryDataset dataset, HistorySearchScope initial_scope) {
    dataset_ = std::move(dataset);
    scope_ = initial_scope;
    index_ = BuildHistorySearchIndex(dataset_, scope_);
    // 开张即列全部(空查询=全部):终端层"查询未变不重跑"的短路在开张那
    // 一帧会因空串==空串跳过 Rerun,这里不先填上,面板就误报"没有命中",
    // 敲一个字才活过来(实测问题 8 验收:Ctrl+R 一开就该显条目)。
    matches_ = SearchHistoryEntries(index_, std::string());
    selected_ = 0;
    active_ = true;
}

void HistorySearchSession::CycleScope() {
    scope_ = NextHistorySearchScope(scope_);
    index_ = BuildHistorySearchIndex(dataset_, scope_);
    matches_.clear();
    selected_ = 0;
}

void HistorySearchSession::Rerun(const std::string& query) {
    matches_ = SearchHistoryEntries(index_, query);
    selected_ = 0;
}

void HistorySearchSession::MoveOlder() {
    if (selected_ + 1 < matches_.size()) {
        ++selected_;
    }
}

void HistorySearchSession::MoveNewer() {
    if (selected_ > 0) {
        --selected_;
    }
}

const PromptHistoryEntry* HistorySearchSession::SelectedEntry() const {
    if (selected_ >= matches_.size()) {
        return nullptr;
    }
    const std::size_t index = matches_[selected_];
    return index < index_.size() ? &index_[index] : nullptr;
}

std::vector<std::string> BuildHistorySearchLines(const HistorySearchSession& session,
                                                 const std::string& query, int width,
                                                 const std::string& highlight_stats,
                                                 const std::string& highlight_reset) {
    std::vector<std::string> lines;
    // 首行:范围 + 可用键。键名从 keymap 反查(用户改绑后提示跟着改,
    // 规格第 9 条"不能写死");某动作没绑键(改绑成空不会发生,但复位到
    // 无默认的 stash 这类不在此列)就整段略过。
    const std::string* scope_word = nullptr;
    switch (session.scope()) {
        case HistorySearchScope::Session: scope_word = &tr("search.scope.session"); break;
        case HistorySearchScope::Project: scope_word = &tr("search.scope.project"); break;
        case HistorySearchScope::All: scope_word = &tr("search.scope.all"); break;
    }
    // 键位串走唯一的 BuildKeyHints 格式化口(收口审计单 §二 P2):与
    // composer 速览左槽同一把尺——未绑定整段略过,和弦/分隔符不另立一套。
    const std::string keys = keymap::BuildKeyHints(
        keymap::ActiveKeymap(),
        {{keymap::ActionId::SearchOlder, "search.key.older"},
         {keymap::ActionId::SearchScopeCycle, "search.key.scope"},
         {keymap::ActionId::SearchAccept, "search.key.accept"},
         {keymap::ActionId::SearchAcceptSubmit, "search.key.accept_submit"},
         {keymap::ActionId::SearchCancel, "search.key.cancel"}});
    lines.push_back(tr("search.header") + " [" + *scope_word + "]" + (keys.empty() ? "" : " · " + keys) +
                    (query.empty() ? std::string() : " · " + tr("search.query") + ": " + query));

    const auto& matches = session.matches();
    if (matches.empty()) {
        lines.push_back("  " + tr("search.no_match"));
        return lines;
    }
    for (std::size_t i = 0; i < matches.size(); ++i) {
        const PromptHistoryEntry& entry = session.index()[matches[i]];
        const bool selected = i == session.selected();
        std::string line = selected ? "❯ " : "  ";
        line += FirstDisplayLine(entry.text);
        // 时间(掐到分)· 标题或项目。
        std::string ts = entry.ts.size() >= 16 ? entry.ts.substr(0, 16) : entry.ts;
        std::string where = entry.title.empty() ? ProjectShortName(entry.project_key) : entry.title;
        const std::string meta = " · " + ts + (where.empty() ? std::string() : " · " + where);
        const std::string prefix_color = selected ? highlight_stats : std::string();
        const std::string reset = highlight_reset;
        // 选中行整行着色(plain 主题色串为空,自然无 ANSI);截宽后收色。
        std::string body = prefix_color + line + reset;
        const int body_width = static_cast<int>(DisplayWidthUtf8(line));
        const int meta_room = width - 2 - body_width - static_cast<int>(DisplayWidthUtf8(meta));
        if (meta_room < 0 && width > 20) {
            // 放不下就先截正文,给 meta 让位(meta 至少留 18 列)。
            const int keep = (std::max)(1, width - 2 - static_cast<int>(DisplayWidthUtf8(meta)) - 1);
            body = prefix_color + TruncateUtf8ToDisplayWidth(line, keep) + reset;
        }
        lines.push_back(body + (selected ? highlight_stats : std::string()) + meta +
                        (selected ? highlight_reset : std::string()));
    }
    return lines;
}

}  // namespace lubancode::cli
