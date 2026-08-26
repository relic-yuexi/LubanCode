// 统一回放接口实现(骨架拆解批五乙,病十六后半)。规矩的正文在这:
// 次序(文件序/seq 稳定排序)、坏行跳过(编解码 nullopt + 折叠口 false/
// 抛异常)、账面(Stats)。域编解码与折叠口各家自带(见 replay.hpp 文件
// 头的接入清单)。

#include "runtime/replay.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::runtime::replay {

namespace {

// 次序规矩(一份):行内带 seq 的域按 seq 稳定排序(journal 的半截尾行
// 跳过后仍单调);不带 seq 的行全 0,stable_sort 即文件序原样。
void OrderBySeq(std::vector<Envelope>& envelopes) {
    std::stable_sort(envelopes.begin(), envelopes.end(),
                     [](const Envelope& a, const Envelope& b) { return a.seq < b.seq; });
}

// 折叠口的兜底:返 false 或抛异常都算"这条不认"。
bool FoldGuarded(const Fold& fold, const Envelope& envelope) {
    try {
        return fold(envelope);
    } catch (...) {
        return false;
    }
}

}  // namespace

Stats ReplayEnvelopes(std::vector<Envelope> envelopes, const Fold& fold) {
    OrderBySeq(envelopes);
    Stats stats;
    stats.lines = static_cast<int>(envelopes.size());
    for (const Envelope& envelope : envelopes) {
        if (FoldGuarded(fold, envelope)) {
            stats.replayed += 1;
        } else {
            stats.skipped += 1;
        }
    }
    return stats;
}

Stats ReplayLedgerLines(const std::vector<std::string>& lines, const LineCodec& codec, const Fold& fold) {
    std::vector<Envelope> envelopes;
    envelopes.reserve(lines.size());
    for (const std::string& line : lines) {
        std::optional<Envelope> envelope;
        try {
            envelope = codec(line);
        } catch (...) {
            envelope.reset();  // 编解码抛异常:坏行,跳过
        }
        if (envelope.has_value()) {
            envelopes.push_back(std::move(*envelope));
        }
    }
    const int parsed = static_cast<int>(envelopes.size());
    Stats stats = ReplayEnvelopes(std::move(envelopes), fold);
    stats.lines = static_cast<int>(lines.size());
    stats.skipped += static_cast<int>(lines.size()) - parsed;  // 解不动的行(异族/坏行)
    return stats;
}

}  // namespace lubancode::runtime::replay
