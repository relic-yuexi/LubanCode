// Unicode emoji 治理单 P1:扩展字素簇(EGC)分段器与列宽策略的纯函数册。
// 铁律(单子 §七 P0):每条断言写死期望簇数/列宽,不拿被测函数自证——
// 期望值全部手写常数,右侧不许出现 DisplayWidth/ClusterDisplayWidth。
// 夹具与单子 §3.1 反例表、§五夹具清单一一对应:ZWJ 序列、VS15/VS16、
// 肤色修饰、旗帜(区域指示符配对)、keycap、组合附标、CJK、孤立零宽。

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "cli/grapheme.hpp"

using lubancode::cli::ClusterDisplayWidth;
using lubancode::cli::SplitGraphemes;
using lubancode::cli::SplitUtf8Graphemes;

namespace {

// 常用夹具(码点序列);构造用初始化列表,不经任何被测函数。
std::u32string U32(std::initializer_list<char32_t> cps) {
    std::u32string out;
    for (char32_t cp : cps) {
        out.push_back(cp);
    }
    return out;
}

std::size_t ClusterCount(const std::u32string& text) {
    return SplitGraphemes(text).size();
}

int TotalWidth(const std::u32string& text) {
    int width = 0;
    for (const auto& cluster : SplitGraphemes(text)) {
        width += cluster.width;
    }
    return width;
}

}  // namespace

// ---------------------------------------------------------------------------
// 一、单子 §3.1 反例表逐行:簇数与列宽(期望值全部写死)
// ---------------------------------------------------------------------------

TEST_CASE("§3.1 反例: e+组合重音是一簇一列,不是两簇两列") {
    const std::u32string text = U32({U'e', 0x0301});
    CHECK(ClusterCount(text) == 1);
    CHECK(TotalWidth(text) == 1);
}

TEST_CASE("§3.1 反例: 👩‍💻 一个 ZWJ 表情,一簇两列") {
    const std::u32string text = U32({0x1F469, 0x200D, 0x1F4BB});
    CHECK(ClusterCount(text) == 1);
    CHECK(TotalWidth(text) == 2);
}

TEST_CASE("§3.1 反例: 👍🏽 带肤色修饰,一簇两列,不是两个可独立编辑的字") {
    const std::u32string text = U32({0x1F44D, 0x1F3FD});
    CHECK(ClusterCount(text) == 1);
    CHECK(TotalWidth(text) == 2);
}

TEST_CASE("§3.1 反例: 👨‍👩‍👧‍👦 家庭组合七码点一簇两列,不是七次编辑步长") {
    const std::u32string text =
        U32({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466});
    CHECK(ClusterCount(text) == 1);
    CHECK(TotalWidth(text) == 2);
}

TEST_CASE("§3.1 反例: 1️⃣ keycap 序列一簇两列") {
    const std::u32string text = U32({U'1', 0xFE0F, 0x20E3});
    CHECK(ClusterCount(text) == 1);
    CHECK(TotalWidth(text) == 2);
}

TEST_CASE("§3.1 反例: ❤️ emoji 呈现两列,❤︎ 文字呈现一列") {
    CHECK(ClusterCount(U32({0x2764, 0xFE0F})) == 1);
    CHECK(TotalWidth(U32({0x2764, 0xFE0F})) == 2);
    CHECK(ClusterCount(U32({0x2764, 0xFE0E})) == 1);
    CHECK(TotalWidth(U32({0x2764, 0xFE0E})) == 1);
}

TEST_CASE("§3.1 反例: 🇨🇳 旗帜两枚指示符一簇两列,不拆成两个编辑单元") {
    const std::u32string flag = U32({0x1F1E8, 0x1F1F3});
    CHECK(ClusterCount(flag) == 1);
    CHECK(TotalWidth(flag) == 2);
}

TEST_CASE("§3.1 反例: 单码点 emoji 与 CJK 行为不回退") {
    CHECK(TotalWidth(U32({0x1F600})) == 2);  // 😀
    CHECK(TotalWidth(U32({U'中'})) == 2);
    CHECK(TotalWidth(U32({U'a', U'中', U'b'})) == 4);
}

// ---------------------------------------------------------------------------
// 二、边界与回退:孤立零宽、三枚指示符、残缺 keycap、行尾 ZWJ
// ---------------------------------------------------------------------------

TEST_CASE("孤立零宽夹具: 孤立附标/ZWJ/VS16 各自一簇,簇宽零") {
    CHECK(ClusterCount(U32({0x0301})) == 1);
    CHECK(TotalWidth(U32({0x0301})) == 0);
    CHECK(ClusterCount(U32({0x200D})) == 1);
    CHECK(TotalWidth(U32({0x200D})) == 0);
    CHECK(ClusterCount(U32({0xFE0F})) == 1);
    CHECK(TotalWidth(U32({0xFE0F})) == 0);
}

TEST_CASE("三枚区域指示符连排: 一对旗帜 + 一枚孤立,两簇宽 2+1") {
    const std::u32string text = U32({0x1F1E8, 0x1F1F3, 0x1F1E8});
    const auto clusters = SplitGraphemes(text);
    REQUIRE(clusters.size() == 2);
    CHECK(clusters[0].begin == 0);
    CHECK(clusters[0].end == 2);
    CHECK(clusters[0].width == 2);
    CHECK(clusters[1].begin == 2);
    CHECK(clusters[1].end == 3);
    CHECK(clusters[1].width == 1);
}

TEST_CASE("残缺 keycap: 无 20E3 帽的 '1'+FE0F 不升两列,保持一列") {
    CHECK(TotalWidth(U32({U'1', 0xFE0F})) == 1);
    // 无 FE0F 的裸 '1'+20E3 也是 keycap(UTS#51 允许省略 VS16)。
    CHECK(ClusterCount(U32({U'1', 0x20E3})) == 1);
    CHECK(TotalWidth(U32({U'1', 0x20E3})) == 2);
}

TEST_CASE("行尾孤立 ZWJ 归前簇;ZWJ 序列后接附标仍是一簇") {
    // "a" + ZWJ(串尾):ZWJ 并入前簇(UAX#29 GB11 尾例)。
    const auto tail = SplitGraphemes(U32({U'a', 0x200D}));
    REQUIRE(tail.size() == 1);
    CHECK(tail[0].end == 2);
    // 👩‍💻 + 组合附标:附标跟着 ZWJ 序列末端并入同一簇。
    CHECK(ClusterCount(U32({0x1F469, 0x200D, 0x1F4BB, 0x0301})) == 1);
}

TEST_CASE("簇首尾相接覆盖整串,空串给空 vector") {
    const std::u32string text = U32({U'a', 0x1F469, 0x200D, 0x1F4BB, U'b'});
    const auto clusters = SplitGraphemes(text);
    REQUIRE(clusters.size() == 3);
    CHECK(clusters[0].begin == 0);
    CHECK(clusters[0].end == 1);
    CHECK(clusters[1].begin == 1);
    CHECK(clusters[1].end == 4);
    CHECK(clusters[2].begin == 4);
    CHECK(clusters[2].end == 5);
    CHECK(SplitGraphemes(U"").empty());
}

TEST_CASE("韩文字母序列与假名浊点并簇") {
    // L + V + T 合成一个音节簇(GB6-8)。
    CHECK(ClusterCount(U32({0x1100, 0x1161, 0x11A8})) == 1);
    // 预成音节 + T 也是一簇。
    CHECK(ClusterCount(U32({0xAC00, 0x11A8})) == 1);
    // あ + 浊点组合成一个视觉字。
    CHECK(ClusterCount(U32({0x3042, 0x3099})) == 1);
    CHECK(TotalWidth(U32({0x3042, 0x3099})) == 2);
}

// ---------------------------------------------------------------------------
// 三、编辑边界:Prev/NextGraphemeBoundary(左右键、退格/Delete 的尺)
// ---------------------------------------------------------------------------

TEST_CASE("编辑边界: 👨‍👩‍👧‍👦 中间位置前后各找最近的簇边界") {
    const std::u32string text = U32({U'x', 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466, U'y'});
    // 码点下标:x=0,家庭=1..7,y=8。光标落在 4(家庭正中间):
    // Prev 给 1(簇首),Next 给 8(下一簇首)。
    CHECK(lubancode::cli::PrevGraphemeBoundary(text, 4) == 1);
    CHECK(lubancode::cli::NextGraphemeBoundary(text, 4) == 8);
    // 端点:0 的 Prev 还是 0;末尾的 Next 给 size()。
    CHECK(lubancode::cli::PrevGraphemeBoundary(text, 0) == 0);
    CHECK(lubancode::cli::NextGraphemeBoundary(text, text.size()) == text.size());
    // 恰在簇首:Prev 给上一簇首,Next 给下一簇首。
    CHECK(lubancode::cli::PrevGraphemeBoundary(text, 1) == 0);
    CHECK(lubancode::cli::NextGraphemeBoundary(text, 1) == 8);
}

TEST_CASE("编辑边界: 组合重音的簇中间不落脚") {
    const std::u32string text = U32({U'e', 0x0301, U'b'});
    CHECK(lubancode::cli::PrevGraphemeBoundary(text, 1) == 0);
    CHECK(lubancode::cli::NextGraphemeBoundary(text, 1) == 2);
}

// ---------------------------------------------------------------------------
// 四、UTF-8 侧:SplitUtf8Graphemes(字节串消费者的同一把尺)
// ---------------------------------------------------------------------------

TEST_CASE("UTF-8 分段: ❤️ 不拆,拼回逐字节相等") {
    const std::string heart = "\xE2\x9D\xA4\xEF\xB8\x8F";  // 2764 FE0F
    const auto glyphs = SplitUtf8Graphemes(heart);
    REQUIRE(glyphs.size() == 1);
    CHECK(glyphs[0].begin == 0);
    CHECK(glyphs[0].len == 6);
    CHECK(glyphs[0].width == 2);
    CHECK(heart.substr(glyphs[0].begin, glyphs[0].len) == heart);
}

TEST_CASE("UTF-8 分段: 家庭组合一簇,与 UTF-32 侧同口径") {
    const std::string family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
    const auto glyphs = SplitUtf8Graphemes(family);
    REQUIRE(glyphs.size() == 1);
    CHECK(glyphs[0].len == family.size());
    CHECK(glyphs[0].width == 2);
}

TEST_CASE("UTF-8 分段: 拆开的坏字节逐字节自立一簇,循环必前进") {
    // ❤️ 的前三个字节(孤立 2764)+ 坏续字节 + 完整 ❤️。
    const std::string torn = "\xE2\x9D\xA4\xB8\x8F\xE2\x9D\xA4\xEF\xB8\x8F";
    const auto glyphs = SplitUtf8Graphemes(torn);
    // 2764(3B)| 坏 B8(1B)| 坏 8F(1B)| 2764(3B)| FE0F(3B 跟随)→ 4 簇。
    REQUIRE(glyphs.size() == 4);
    CHECK(glyphs[0].len == 3);
    CHECK(glyphs[1].len == 1);
    CHECK(glyphs[1].width == 1);  // 坏字节回退簇记一列
    CHECK(glyphs[2].len == 1);
    CHECK(glyphs[3].len == 6);  // 2764+FE0F 重新合成一簇
    CHECK(glyphs[3].width == 2);
    std::size_t covered = 0;
    for (const auto& glyph : glyphs) {
        covered += glyph.len;
        CHECK(glyph.len >= 1);  // 索引必前进(§3.3 铁律)
    }
    CHECK(covered == torn.size());
}

TEST_CASE("UTF-8 分段: 串尾截断的多字节序列按坏字节回退") {
    // 👩 的前两字节(截断)+ 'a':F0 等不到合法续字节、9F 是坏首字节,
    // 各自一簇,' ' 与 'a' 各一簇。
    const std::string torn = "\xF0\x9F a";
    const auto glyphs = SplitUtf8Graphemes(torn);
    REQUIRE(glyphs.size() == 4);
    CHECK(glyphs[0].len == 1);
    CHECK(glyphs[1].len == 1);
    CHECK(glyphs[2].len == 1);
    CHECK(glyphs[2].width == 1);
    CHECK(glyphs[3].len == 1);
}

TEST_CASE("ClusterDisplayWidth(string_view): 与指针版同账") {
    CHECK(ClusterDisplayWidth(std::u32string_view(U32({0x1F1E8, 0x1F1F3}))) == 2);
    CHECK(ClusterDisplayWidth(std::u32string_view(U32({U'e', 0x0301}))) == 1);
    CHECK(ClusterDisplayWidth(std::u32string_view()) == 0);
}

TEST_CASE("宽度策略表: 附标/ZWJ/VS/肤色单宽为零,孤立指示符一列") {
    CHECK(lubancode::cli::GraphemeCodepointWidth(0x0301) == 0);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0x200D) == 0);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0xFE0F) == 0);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0xFE0E) == 0);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0x1F3FD) == 0);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0x1F1E8) == 1);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0x2764) == 1);
    CHECK(lubancode::cli::GraphemeCodepointWidth(U'a') == 1);
    CHECK(lubancode::cli::GraphemeCodepointWidth(0) == 0);
}
