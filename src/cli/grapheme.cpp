// 扩展字素簇分段与终端列宽——实现。数据口径与覆盖边界:
//
// Unicode 15.1(UCD 15.1)。两张内嵌表都是人工从 15.1 的
// Grapheme_Cluster_Break(Extend/Regional_Indicator/ZWJ)与 East Asian
// Width + Emoji_Presentation(宽字)摘录的紧凑区间表,不是全量 UAX#29
// 属性表。摘录原则:
//   - Extend:覆盖使用人口前列文字系统(拉丁/希腊/西里尔/希伯来/阿拉伯/
//     天城/孟加拉/泰米尔/泰卢固/卡纳达/泰/老挝/缅甸/高棉/藏文/假名浊点/
//     变体选择符/emoji 肤色修饰)的常用组合附标;考古文字(圣书体、线性
//     A 等)的细粒度附标未列。
//   - 宽字:既有 CJK 区段(line_editor 老表原样搬来,行为不回退)+ 常用
//     emoji 呈现区段(Emoji_Presentation 主体)。
// 未覆盖区间的回退:按 Base(独立簇)与一列处理——与升级前的行为一致,
// 最坏情形只是"该文字的附标被当独立一字",不会错误合并、不会死循环。
//
// 表若与真实 UCD 有出入,影响面是生僻区段 ±1 列/簇边界;单子 §3.1 的
// 全部反例(组合重音、ZWJ 序列、肤色、旗帜、keycap、VS15/16、CJK)都
// 在覆盖面内,由 tests/unit/cli/test_grapheme.cpp 用独立预期值钉死。

#include "cli/grapheme.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace lubancode::cli {
namespace {

// ---- 表一:Grapheme_Extend(含变体选择符与肤色修饰)区间,15.1 摘录 ----
// 严格按码位升序排;static_assert 把关。
constexpr std::pair<char32_t, char32_t> kExtendRanges[] = {
    {0x0300, 0x036F},    // 组合附标(é 这类)
    {0x0483, 0x0489},    // 西里尔附标
    {0x0591, 0x05BD},    // 希伯来点符
    {0x05BF, 0x05BF},
    {0x05C1, 0x05C2},
    {0x05C4, 0x05C5},
    {0x05C7, 0x05C7},
    {0x0610, 0x061A},    // 阿拉伯
    {0x064B, 0x065F},
    {0x0670, 0x0670},
    {0x06D6, 0x06DC},
    {0x06DF, 0x06E4},
    {0x06E7, 0x06E8},
    {0x06EA, 0x06ED},
    {0x0711, 0x0711},    // 叙利亚
    {0x0730, 0x074A},
    {0x07A6, 0x07B0},    // 塔安那/恩科
    {0x07EB, 0x07F3},
    {0x0816, 0x0819},    // 撒玛利亚
    {0x081B, 0x0823},
    {0x0825, 0x0827},
    {0x0829, 0x082D},
    {0x0859, 0x085B},    // 曼达安
    {0x08D3, 0x08E1},    // 阿拉伯扩展
    {0x08E3, 0x0902},    // 天城文头
    {0x093A, 0x093A},    // 天城文
    {0x093C, 0x093C},
    {0x0941, 0x0948},
    {0x094D, 0x094D},
    {0x0951, 0x0957},
    {0x0962, 0x0963},
    {0x0981, 0x0981},    // 孟加拉
    {0x09BC, 0x09BC},
    {0x09C1, 0x09C4},
    {0x09CD, 0x09CD},
    {0x09E2, 0x09E3},
    {0x0A01, 0x0A02},    // 旁遮普
    {0x0A3C, 0x0A3C},
    {0x0A41, 0x0A42},
    {0x0A47, 0x0A48},
    {0x0A4B, 0x0A4D},
    {0x0A51, 0x0A51},
    {0x0A70, 0x0A71},
    {0x0A75, 0x0A75},
    {0x0B01, 0x0B01},    // 奥里亚
    {0x0B3C, 0x0B3C},
    {0x0B3F, 0x0B3F},
    {0x0B41, 0x0B44},
    {0x0B4D, 0x0B4D},
    {0x0B56, 0x0B56},
    {0x0B62, 0x0B63},
    {0x0B82, 0x0B82},    // 泰米尔
    {0x0BC0, 0x0BC0},
    {0x0BCD, 0x0BCD},
    {0x0C00, 0x0C00},    // 泰卢固
    {0x0C3E, 0x0C40},
    {0x0C46, 0x0C48},
    {0x0C4A, 0x0C4D},
    {0x0C81, 0x0C81},    // 卡纳达
    {0x0CBC, 0x0CBC},
    {0x0CBF, 0x0CBF},
    {0x0CC6, 0x0CC6},
    {0x0CCC, 0x0CCD},
    {0x0D01, 0x0D01},    // 马拉雅拉姆
    {0x0D41, 0x0D44},
    {0x0D4D, 0x0D4D},
    {0x0DCA, 0x0DCA},    // 僧伽罗
    {0x0DDF, 0x0DDF},
    {0x0E31, 0x0E31},    // 泰文
    {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1},    // 老挝
    {0x0EB4, 0x0EBC},
    {0x0EC8, 0x0ECD},
    {0x0F71, 0x0F84},    // 藏文(主段)
    {0x0F86, 0x0F87},
    {0x102D, 0x1030},    // 缅甸
    {0x1032, 0x1037},
    {0x1039, 0x103A},
    {0x103D, 0x103E},
    {0x1058, 0x1059},
    {0x105E, 0x1060},
    {0x17B4, 0x17B5},    // 高棉(可见附标)
    {0x17B7, 0x17BD},
    {0x17C6, 0x17C6},
    {0x17C9, 0x17D3},
    {0x180B, 0x180D},    // 蒙古自由变体选择符
    {0x18A9, 0x18A9},
    {0x1AB0, 0x1ACE},    // 组合附标扩展
    {0x1B00, 0x1B03},    // 巴厘
    {0x1B34, 0x1B34},
    {0x1DC0, 0x1DFF},    // 组合附标补充
    {0x20D0, 0x20F0},    // 符号用组合记号(含 20E3 keycap 帽)
    {0x2CEF, 0x2CF1},    // 科普特
    {0x2D7F, 0x2D7F},    // 提非纳连接符
    {0x2DE0, 0x2DFF},    // 西里尔扩展 A
    {0x302A, 0x302F},    // CJK 附标
    {0x3099, 0x309A},    // 假名浊点
    {0xA66F, 0xA672},    // 西里尔扩展 B
    {0xA674, 0xA67D},
    {0xA69E, 0xA69F},
    {0xA6F0, 0xA6F1},    // 巴姆穆
    {0xA802, 0xA802},    // 苏莱特纳格里
    {0xA806, 0xA806},
    {0xA80B, 0xA80B},
    {0xA825, 0xA826},
    {0xA8C4, 0xA8C5},
    {0xA8E0, 0xA8F1},    // 天城扩展
    {0xA926, 0xA92D},
    {0xA947, 0xA951},
    {0xA980, 0xA982},
    {0xA9B3, 0xA9B3},
    {0xA9B6, 0xA9B9},
    {0xA9BC, 0xA9BD},
    {0xA9E5, 0xA9E5},
    {0xAA29, 0xAA2E},    // 占语
    {0xAA31, 0xAA32},
    {0xAA35, 0xAA36},
    {0xAA43, 0xAA43},
    {0xAA4C, 0xAA4C},
    {0xAA7C, 0xAA7C},
    {0xAAB0, 0xAAB0},    // 岱塔越
    {0xAAB2, 0xAAB4},
    {0xAAB7, 0xAAB8},
    {0xAABE, 0xAABF},
    {0xAAC1, 0xAAC1},
    {0xAAEC, 0xAAED},
    {0xAAF6, 0xAAF6},
    {0xABE5, 0xABE5},
    {0xABE8, 0xABE8},
    {0xABED, 0xABED},
    {0xFB1E, 0xFB1E},    // 希伯来点
    {0xFE00, 0xFE0F},    // 变体选择符(VS1..VS16)
    {0xFE20, 0xFE2F},    // 组合半记号
    {0x101FD, 0x101FD},  // 斐斯托斯
    {0x10A38, 0x10A3A},  // 佉卢
    {0x11001, 0x11001},  // 婆罗米
    {0x11038, 0x11046},
    {0x110B3, 0x110B6},
    {0x110B9, 0x110BA},
    {0x11100, 0x11102},
    {0x11127, 0x11134},
    {0x11173, 0x11173},
    {0x11180, 0x11181},
    {0x111B6, 0x111BE},
    {0x1122F, 0x11231},
    {0x11234, 0x11234},
    {0x11236, 0x11237},
    {0x112DF, 0x112DF},
    {0x112E3, 0x112EA},
    {0x11300, 0x11301},
    {0x1133B, 0x1133C},
    {0x11366, 0x1136C},
    {0x11370, 0x11374},
    {0x11438, 0x1143F},
    {0x11442, 0x11444},
    {0x11446, 0x11446},
    {0x114B3, 0x114B8},
    {0x114BA, 0x114BA},
    {0x114BF, 0x114C0},
    {0x114C2, 0x114C3},
    {0x115B2, 0x115B5},
    {0x115BC, 0x115BD},
    {0x115BF, 0x115C0},
    {0x115DC, 0x115DD},
    {0x11633, 0x1163A},
    {0x1163D, 0x1163D},
    {0x1163F, 0x11640},
    {0x116AB, 0x116AB},
    {0x116AD, 0x116AD},
    {0x116B0, 0x116B5},
    {0x116B7, 0x116B7},
    {0x1171D, 0x1171F},
    {0x11722, 0x11725},
    {0x11727, 0x1172B},
    {0x16AF0, 0x16AF4},  // 巴萨瓦
    {0x16B30, 0x16B36},  // 苗文
    {0x16F4F, 0x16F4F},
    {0x16F8F, 0x16F92},
    {0x1BC9D, 0x1BC9E},  // 杜普洛扬
    {0x1D165, 0x1D169},  // 音乐记谱
    {0x1D16D, 0x1D172},
    {0x1D17B, 0x1D182},
    {0x1D185, 0x1D18B},
    {0x1D1AA, 0x1D1AD},
    {0x1D242, 0x1D244},
    {0x1DA00, 0x1DA36},  // 手语
    {0x1DA3B, 0x1DA6C},
    {0x1DA75, 0x1DA75},
    {0x1DA84, 0x1DA84},
    {0x1E000, 0x1E006},  // 格拉哥里补充
    {0x1E008, 0x1E018},
    {0x1E01B, 0x1E021},
    {0x1E023, 0x1E024},
    {0x1E026, 0x1E02A},
    {0x1E130, 0x1E136},
    {0x1E2EC, 0x1E2EF},
    {0x1E8D0, 0x1E8D6},
    {0x1E944, 0x1E94A},
    {0x1F3FB, 0x1F3FF},  // emoji 肤色修饰(15.1 Emoji_Modifier)
    {0xE0100, 0xE01EF},  // 变体选择符补充
};

// ---- 表二:终端宽字(East Asian Width W/F + Emoji_Presentation) ----
// 既有 CJK 区段(line_editor 老表原样搬来,行为不回退)+ 常用 emoji 呈现
// 区段(15.1 Emoji_Presentation 主体,默认 emoji 呈现、占两列)。严格按
// 码位升序排;static_assert 把关。
constexpr std::pair<char32_t, char32_t> kWideRanges[] = {
    {0x1100, 0x115F},    // 韩文字母(Hangul Jamo)
    {0x231A, 0x231B},    // 手表/热饮(离散 emoji 呈现字,EAW=W)
    {0x2329, 0x232A},
    {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},
    {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},
    {0x2614, 0x2615},
    {0x2648, 0x2653},    // 黄道十二宫
    {0x267F, 0x267F},
    {0x2693, 0x2693},
    {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},
    {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},
    {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},
    {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},
    {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},
    {0x26FD, 0x26FD},
    {0x2705, 0x2705},
    {0x270A, 0x270B},
    {0x2728, 0x2728},
    {0x274C, 0x274C},
    {0x274E, 0x274E},
    {0x2753, 0x2755},
    {0x2757, 0x2757},
    {0x2795, 0x2797},
    {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x2E80, 0x303E},    // CJK 部首、CJK 标点
    {0x3041, 0x33FF},    // 平假名、片假名、CJK 兼容
    {0x3400, 0x4DBF},    // CJK 扩展 A
    {0x4E00, 0x9FFF},    // CJK 统一表意文字
    {0xA000, 0xA4CF},    // 彝文
    {0xA960, 0xA97C},    // 韩文字母扩展 A
    {0xAC00, 0xD7A3},    // 韩文音节
    {0xF900, 0xFAFF},    // CJK 兼容表意文字
    {0xFE10, 0xFE19},    // 竖排标点
    {0xFE30, 0xFE6F},    // CJK 兼容形式
    {0xFF00, 0xFF60},    // 全角字符
    {0xFFE0, 0xFFE6},
    {0x16FE0, 0x16FE4},  // 表意文字描述及扩展
    {0x17000, 0x18AFF},  // 西夏/小契丹(连续宽区)
    {0x1B000, 0x1B2FF},  // 假名补充
    {0x1F004, 0x1F004},  // ---- Emoji_Presentation 主体(15.1) ----
    {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F200, 0x1F2FF},  // 围圈表意补充(全宽)
    {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7},
    {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945},
    {0x1F947, 0x1F978},
    {0x1F97A, 0x1F9CB},
    {0x1F9CD, 0x1F9FF},
    {0x1FA70, 0x1FA7C},
    {0x1FA80, 0x1FA88},
    {0x1FA90, 0x1FABD},
    {0x1FABF, 0x1FAC5},
    {0x1FACE, 0x1FADC},
    {0x1FADF, 0x1FAE8},
    {0x1FAF0, 0x1FAF8},
    {0x20000, 0x2FFFD},  // CJK 扩展 B 及以上
    {0x30000, 0x3FFFD},
};

// 两张表都是严格递增区间,static_assert 把关(手写表最容易犯的错)。
constexpr bool kExtendSorted = [] {
    for (std::size_t i = 1; i < std::size(kExtendRanges); ++i) {
        if (kExtendRanges[i - 1].second >= kExtendRanges[i].first) {
            return false;
        }
    }
    return true;
}();
static_assert(kExtendSorted, "kExtendRanges 必须严格递增,否则二分失真");

constexpr bool kWideSorted = [] {
    for (std::size_t i = 1; i < std::size(kWideRanges); ++i) {
        if (kWideRanges[i - 1].second >= kWideRanges[i].first) {
            return false;
        }
    }
    return true;
}();
static_assert(kWideSorted, "kWideRanges 必须严格递增,否则二分失真");

template <std::size_t N>
bool InRanges(const std::pair<char32_t, char32_t> (&ranges)[N], char32_t cp) {
    std::size_t lo = 0;
    std::size_t hi = N;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first) {
            hi = mid;
        } else if (cp > ranges[mid].second) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

constexpr char32_t kZwj = 0x200D;
constexpr char32_t kVs15 = 0xFE0E;
constexpr char32_t kVs16 = 0xFE0F;
constexpr char32_t kKeycapCap = 0x20E3;

bool IsRegionalIndicator(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

bool IsKeycapBase(char32_t cp) {
    return (cp >= U'0' && cp <= U'9') || cp == U'#' || cp == U'*';
}

bool IsHangulJamoClass(GraphemeClass cls) {
    return cls == GraphemeClass::HangulL || cls == GraphemeClass::HangulV ||
           cls == GraphemeClass::HangulT || cls == GraphemeClass::HangulSyllable;
}

}  // namespace

GraphemeClass ClassifyCodepoint(char32_t cp) {
    if (cp == kZwj) {
        return GraphemeClass::Zwj;
    }
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
        InRanges(kExtendRanges, cp)) {
        return GraphemeClass::Extend;
    }
    if (IsRegionalIndicator(cp)) {
        return GraphemeClass::RegionalIndicator;
    }
    if (IsKeycapBase(cp)) {
        return GraphemeClass::KeycapBase;
    }
    if (cp >= 0x1100 && cp <= 0x115F) {
        return GraphemeClass::HangulL;  // Jamo 起始段(L 与兼容 V 起点)
    }
    if (cp >= 0x1160 && cp <= 0x11A7) {
        return GraphemeClass::HangulV;
    }
    if (cp >= 0x11A8 && cp <= 0x11FF) {
        return GraphemeClass::HangulT;
    }
    if (cp >= 0xA960 && cp <= 0xA97C) {
        return GraphemeClass::HangulL;  // 扩展 A 按 L 对待(EGC 并簇口径)
    }
    if (cp >= 0xD7B0 && cp <= 0xD7C6) {
        return GraphemeClass::HangulV;  // Jamo 扩展 B 前半
    }
    if (cp >= 0xD7CB && cp <= 0xD7FB) {
        return GraphemeClass::HangulT;  // Jamo 扩展 B 后半
    }
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        return GraphemeClass::HangulSyllable;
    }
    return GraphemeClass::Base;
}

int GraphemeCodepointWidth(char32_t cp) {
    if (cp == 0) {
        return 0;
    }
    const GraphemeClass cls = ClassifyCodepoint(cp);
    if (cls == GraphemeClass::Extend || cls == GraphemeClass::Zwj) {
        return 0;  // 附标/VS/ZWJ/肤色:零宽跟随者,孤立出现也不占列
    }
    if (cls == GraphemeClass::RegionalIndicator) {
        return 1;  // 孤立指示符一列;配对成旗帜由簇宽升两列
    }
    if (InRanges(kWideRanges, cp)) {
        return 2;
    }
    return 1;
}

int ClusterDisplayWidth(const char32_t* cps, std::size_t count) {
    if (cps == nullptr || count == 0) {
        return 0;
    }
    const char32_t base = cps[0];
    if (base == kZwj || ClassifyCodepoint(base) == GraphemeClass::Extend) {
        // 孤立零宽开头:整簇零宽(可见回退由展示层负责,量宽不虚报)。
        return 0;
    }
    bool has_vs15 = false;
    bool has_vs16 = false;
    for (std::size_t i = 1; i < count; ++i) {
        if (cps[i] == kVs15) {
            has_vs15 = true;
        } else if (cps[i] == kVs16) {
            has_vs16 = true;
        }
    }
    // 旗帜:两枚区域指示符配对,整簇两列(主流终端把 🇨🇳 画两列)。
    if (count == 2 && IsRegionalIndicator(base) && IsRegionalIndicator(cps[1])) {
        return 2;
    }
    // keycap:帽子 20E3 收尾,整簇两列(1️⃣ 在终端里占两格)。
    if (count >= 2 && cps[count - 1] == kKeycapCap) {
        return 2;
    }
    if (has_vs15) {
        return 1;  // 文字呈现请求:降一列(❤︎)
    }
    if (has_vs16) {
        // emoji 呈现请求:升两列(❤️)。ASCII/拉丁区的基字没有 emoji 呈现
        // 形态,终端也不给两列,'1'+FE0F 这类残缺序列保持基字宽。
        return base >= 0x2000 ? 2 : GraphemeCodepointWidth(base);
    }
    return GraphemeCodepointWidth(base);  // ZWJ 序列/肤色/附标全取首字宽
}

int ClusterDisplayWidth(std::u32string_view cluster) {
    return ClusterDisplayWidth(cluster.data(), cluster.size());
}

std::vector<GraphemeCluster> SplitGraphemes(const std::u32string& text) {
    std::vector<GraphemeCluster> clusters;
    const std::size_t n = text.size();
    std::size_t pos = 0;
    while (pos < n) {
        const std::size_t start = pos;
        const GraphemeClass head = ClassifyCodepoint(text[pos]);
        ++pos;
        if (head == GraphemeClass::RegionalIndicator) {
            // GB12/13:指示符两两配对成旗帜(简化:只收一对;三个连排时
            // 第三枚开新簇——UAX#29 也按对分)。
            if (pos < n && ClassifyCodepoint(text[pos]) == GraphemeClass::RegionalIndicator) {
                ++pos;
            }
        } else if (head == GraphemeClass::KeycapBase) {
            // keycap:数字/#/* + FE0F? + 20E3。
            if (pos < n && text[pos] == kVs16) {
                ++pos;
            }
            if (pos < n && text[pos] == kKeycapCap) {
                ++pos;
            }
        } else if (IsHangulJamoClass(head)) {
            // GB6-8:韩文字母序列并簇;预成音节后仍可跟 T。
            while (pos < n && IsHangulJamoClass(ClassifyCodepoint(text[pos]))) {
                ++pos;
            }
        }
        // GB9/GB11:附标、VS、肤色并入;ZWJ 并入且把下一字符拉进簇
        //(严格 GB11 限于 ExtPict 前后,这里简化为无条件粘连——普通文字
        // 夹 ZWJ 的输入极罕见,粘连只影响编辑步长,不产生错误字节)。
        while (pos < n) {
            const GraphemeClass cls = ClassifyCodepoint(text[pos]);
            if (cls == GraphemeClass::Extend) {
                ++pos;
                continue;
            }
            if (cls == GraphemeClass::Zwj) {
                if (pos + 1 < n) {
                    pos += 2;  // ZWJ + 下一字符,整段并入
                    continue;
                }
                ++pos;  // 行尾孤立 ZWJ:归前簇(UAX#29 GB11 尾例)
                break;
            }
            break;
        }
        clusters.push_back(GraphemeCluster{
            start, pos, ClusterDisplayWidth(text.data() + start, pos - start)});
    }
    return clusters;
}

std::vector<Utf8Grapheme> SplitUtf8Graphemes(std::string_view utf8) {
    // 按字节宽松解码出码点(与 Utf8ToUtf32 同口径:坏首字节/截断序列按
    // "跳一个字节"处理),再按 SplitGraphemes 同一套并簇规则分组。实现
    // 成"逐码点重放分类":新码点会不会开新簇,由它与前一枚码点的类别
    // 关系决定,与 SplitGraphemes 的状态机逐条对齐。坏字节自立一簇
    // (宽 1)——索引必前进(单子 §3.3 铁律)。
    std::vector<Utf8Grapheme> out;
    const std::size_t n = utf8.size();
    std::u32string buf;         // 当前簇攒下的码点
    std::size_t buf_begin = 0;  // 当前簇在 utf8 里的字节起点
    const auto flush = [&](std::size_t end_byte) {
        if (buf.empty()) {
            return;
        }
        out.push_back(Utf8Grapheme{buf_begin, end_byte - buf_begin,
                                   ClusterDisplayWidth(buf.data(), buf.size())});
        buf.clear();
    };
    std::size_t i = 0;
    bool after_zwj = false;  // 上一枚是"主动拉动"的 ZWJ:下一码点无条件并入
    while (i < n) {
        const unsigned char lead = static_cast<unsigned char>(utf8[i]);
        std::size_t len = 1;
        char32_t cp = lead;
        bool bad = false;
        if (lead >= 0x80) {
            if ((lead & 0xE0) == 0xC0) {
                cp = lead & 0x1F;
                len = 2;
            } else if ((lead & 0xF0) == 0xE0) {
                cp = lead & 0x0F;
                len = 3;
            } else if ((lead & 0xF8) == 0xF0) {
                cp = lead & 0x07;
                len = 4;
            } else {
                bad = true;
            }
            if (!bad) {
                for (std::size_t k = 1; k < len; ++k) {
                    if (i + k >= n) {
                        bad = true;  // 串尾截断
                        break;
                    }
                    const unsigned char cont = static_cast<unsigned char>(utf8[i + k]);
                    if ((cont & 0xC0) != 0x80) {
                        bad = true;
                        break;
                    }
                    cp = (cp << 6) | (cont & 0x3F);
                }
            }
        }
        if (bad) {
            flush(i);  // 先冲掉手头攒的簇,坏字节自立一簇
            out.push_back(Utf8Grapheme{i, 1, 1});
            buf_begin = i + 1;
            ++i;
            continue;
        }
        bool starts_new = false;
        const GraphemeClass cls = ClassifyCodepoint(cp);
        if (!buf.empty()) {
            const GraphemeClass prev_cls = ClassifyCodepoint(buf.back());
            if (cls == GraphemeClass::Extend) {
                starts_new = false;
            } else if (cls == GraphemeClass::Zwj) {
                starts_new = false;
            } else if (after_zwj) {
                starts_new = false;  // 被 ZWJ 拉进来的那一枚(双 ZWJ 不连拉,
                                     // 与 SplitGraphemes 的 pos+=2 消化同口径)
            } else if (IsHangulJamoClass(cls) && IsHangulJamoClass(prev_cls)) {
                starts_new = false;
            } else if (cls == GraphemeClass::RegionalIndicator &&
                       prev_cls == GraphemeClass::RegionalIndicator && buf.size() == 1) {
                starts_new = false;  // 旗帜第二枚
            } else {
                starts_new = true;
            }
        }
        // ZWJ 主动拉下一枚;自己就是被拉进来的 ZWJ(双 ZWJ 的第二枚)不再拉。
        after_zwj = cls == GraphemeClass::Zwj && !after_zwj;
        if (starts_new) {
            flush(i);
            buf_begin = i;
        }
        buf.push_back(cp);
        i += len;
    }
    flush(n);
    return out;
}

std::size_t PrevGraphemeBoundary(const std::u32string& text, std::size_t pos) {
    if (pos == 0 || text.empty()) {
        return 0;
    }
    if (pos > text.size()) {
        pos = text.size();
    }
    const std::vector<GraphemeCluster> clusters = SplitGraphemes(text);
    for (std::size_t i = clusters.size(); i > 0; --i) {
        if (clusters[i - 1].begin < pos) {
            return clusters[i - 1].begin;
        }
    }
    return 0;
}

std::size_t NextGraphemeBoundary(const std::u32string& text, std::size_t pos) {
    if (pos >= text.size()) {
        return text.size();
    }
    const std::vector<GraphemeCluster> clusters = SplitGraphemes(text);
    for (const GraphemeCluster& cluster : clusters) {
        if (cluster.begin > pos) {
            return cluster.begin;
        }
    }
    return text.size();
}

}  // namespace lubancode::cli
