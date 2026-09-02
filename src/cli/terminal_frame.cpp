#include "cli/terminal_frame.hpp"

#include <algorithm>
#include <cstdlib>

#include "cli/line_editor.hpp"

namespace lubancode::cli {

WrappedComposerLayout LayoutComposerRows(const std::vector<std::u32string>& logical_lines,
                                         std::size_t cursor_row, std::size_t cursor_col,
                                         int first_width, int continuation_width) {
    const std::vector<std::u32string> fallback{std::u32string()};
    const auto& lines = logical_lines.empty() ? fallback : logical_lines;
    const std::size_t safe_cursor_row = (std::min)(cursor_row, lines.size() - 1);
    const std::size_t safe_cursor_col = (std::min)(cursor_col, lines[safe_cursor_row].size());

    WrappedComposerLayout layout;
    std::vector<std::pair<std::size_t, std::size_t>> row_ranges(lines.size());
    for (std::size_t logical = 0; logical < lines.size(); ++logical) {
        const std::u32string& line = lines[logical];
        const std::size_t first_row = layout.rows.size();
        if (line.empty()) {
            layout.rows.push_back(WrappedComposerRow{{}, logical, 0, 0, 0});
        } else {
            std::size_t begin = 0;
            while (begin < line.size()) {
                const bool first_physical_row = layout.rows.empty();
                const int capacity = (std::max)(1, first_physical_row ? first_width : continuation_width);
                std::size_t end = begin;
                int width = 0;
                while (end < line.size()) {
                    const int char_width = CharDisplayWidth(line[end]);
                    if (end > begin && width + char_width > capacity) {
                        break;
                    }
                    width += char_width;
                    ++end;
                    if (width >= capacity) {
                        break;
                    }
                }
                layout.rows.push_back(
                    WrappedComposerRow{line.substr(begin, end - begin), logical, begin, end, width});
                begin = end;
            }
        }
        row_ranges[logical] = {first_row, layout.rows.size()};
    }

    const auto [first_row, row_end] = row_ranges[safe_cursor_row];
    for (std::size_t physical = first_row; physical < row_end; ++physical) {
        const WrappedComposerRow& row = layout.rows[physical];
        const bool last = physical + 1 == row_end;
        if (safe_cursor_col < row.source_end || last) {
            layout.cursor_row = physical;
            const std::size_t relative = safe_cursor_col >= row.source_begin
                                             ? safe_cursor_col - row.source_begin
                                             : 0;
            layout.cursor_col = static_cast<int>(DisplayWidth(row.text.substr(0, relative)));
            break;
        }
    }
    return layout;
}

namespace {

// 行级 diff 的纯计算核(单 2 二轮抽出):哪些行变了、每行清写区间(旧行/
// 新行取并集)与硬度、新文本从哪格写起。VT 批(QueueInlineFrameDiff)与
// 原生直写(PaintInlineFrameNativeRows)共用同一把账,两条写路看到的
// "哪行脏"永远一致;字节合同由既有的帧测试钉死。
struct InlineRowChange {
    std::size_t row = 0;   // 帧内行号
    int clear_x = 0;       // 清写起点(含)
    int clear_end = 0;     // 清写终点(不含)
    bool hard_clear = false;
    int write_x = -1;      // -1 = 只清不写(新行没有文本)
    std::string_view text; // write_x >= 0 时有效
};

std::vector<InlineRowChange> ComputeInlineRowChanges(const InlineFrame* previous, const InlineFrame& next) {
    const std::size_t old_size = previous == nullptr ? 0 : previous->rows.size();
    const std::size_t compared = (std::max)(old_size, next.rows.size());
    std::vector<InlineRowChange> changes;
    for (std::size_t i = 0; i < compared; ++i) {
        const InlineFrameRow* old_row = i < old_size ? &previous->rows[i] : nullptr;
        const InlineFrameRow* new_row = i < next.rows.size() ? &next.rows[i] : nullptr;
        if (old_row != nullptr && new_row != nullptr && *old_row == *new_row) {
            continue;
        }
        InlineRowChange change;
        change.row = i;
        int clear_x = new_row != nullptr ? new_row->x : old_row->x;
        int clear_end = clear_x + (new_row != nullptr ? new_row->clear_width : old_row->clear_width);
        change.hard_clear =
            (new_row != nullptr && new_row->hard_clear) || (old_row != nullptr && old_row->hard_clear);
        if (old_row != nullptr) {
            clear_x = (std::min)(clear_x, old_row->x);
            clear_end = (std::max)(clear_end, old_row->x + old_row->clear_width);
        }
        change.clear_x = clear_x;
        change.clear_end = clear_end;
        if (new_row != nullptr && !new_row->text.empty()) {
            change.write_x = new_row->x;
            change.text = new_row->text;
        }
        changes.push_back(change);
    }
    return changes;
}

// ---- SGR -> 16 色属性位(BuildNativeRowCells 的翻译件,纯函数) ----------

constexpr std::uint16_t kNativeAttrFgMask = 0x0F;
constexpr std::uint16_t kNativeAttrBgMask = 0xF0;

// xterm-256 调色板取 RGB:0-15 标准色、16-231 是 6^3 立方、232-255 灰阶。
// footer 主题今天不用 256 色(diff/surface 色只进转录区),这张表是给未来
// 添色时兜底的——近似到最近 16 色,肉眼看得出偏差也远好于整行丢色。
struct RgbTriplet {
    int r;
    int g;
    int b;
};

RgbTriplet Xterm256Rgb(int idx) {
    if (idx < 16) {
        static const RgbTriplet kBase[16] = {
            {0, 0, 0},         {205, 0, 0},       {0, 205, 0},       {205, 205, 0},
            {0, 0, 238},       {205, 0, 205},     {0, 205, 205},     {229, 229, 229},
            {127, 127, 127},   {255, 0, 0},       {0, 255, 0},       {255, 255, 0},
            {92, 92, 255},     {255, 0, 255},     {0, 255, 255},     {255, 255, 255},
        };
        return kBase[idx & 15];
    }
    if (idx < 232) {
        static const int kCube[6] = {0, 95, 135, 175, 215, 255};
        const int v = idx - 16;
        return RgbTriplet{kCube[(v / 36) % 6], kCube[(v / 6) % 6], kCube[v % 6]};
    }
    const int gray = 8 + 10 * (idx - 232);
    return RgbTriplet{gray, gray, gray};
}

// ANSI 30-37/90-97 的色号 -> 前景位(前景一档;背景版由 AnsiBgBits 平移)。
// platform:: 的常量在这只匿名段里都得带着姓——cli 里没开 using。
std::uint16_t AnsiFgBits(int color, bool bright) {
    namespace plat = lubancode::platform;
    switch (color & 7) {
        case 0:
            return bright ? plat::kNativeFgIntensity : 0;
        case 1:
            return bright ? (plat::kNativeFgRed | plat::kNativeFgIntensity) : plat::kNativeFgRed;
        case 2:
            return bright ? (plat::kNativeFgGreen | plat::kNativeFgIntensity) : plat::kNativeFgGreen;
        case 3:
            return bright ? (plat::kNativeFgRed | plat::kNativeFgGreen | plat::kNativeFgIntensity)
                          : static_cast<std::uint16_t>(plat::kNativeFgRed | plat::kNativeFgGreen);
        case 4:
            return bright ? (plat::kNativeFgBlue | plat::kNativeFgIntensity) : plat::kNativeFgBlue;
        case 5:
            return bright ? (plat::kNativeFgRed | plat::kNativeFgBlue | plat::kNativeFgIntensity)
                          : static_cast<std::uint16_t>(plat::kNativeFgRed | plat::kNativeFgBlue);
        case 6:
            return bright ? (plat::kNativeFgGreen | plat::kNativeFgBlue | plat::kNativeFgIntensity)
                          : static_cast<std::uint16_t>(plat::kNativeFgGreen | plat::kNativeFgBlue);
        default:
            return bright
                       ? (plat::kNativeFgRed | plat::kNativeFgGreen | plat::kNativeFgBlue | plat::kNativeFgIntensity)
                       : static_cast<std::uint16_t>(plat::kNativeFgRed | plat::kNativeFgGreen | plat::kNativeFgBlue);
    }
}

std::uint16_t AnsiBgBits(int color, bool bright) {
    const std::uint16_t fg = AnsiFgBits(color, bright);
    // 色号同构:前景位右移到背景位(1/2/4/8 -> 16/32/64/128)。
    return static_cast<std::uint16_t>((fg & 0x0F) << 4);
}

std::uint16_t NearestNativeBits(const RgbTriplet& rgb, bool background) {
    int best = 0;
    int best_dist = 1 << 30;
    for (int i = 0; i < 16; ++i) {
        const RgbTriplet cand = Xterm256Rgb(i);
        const int dist = std::abs(cand.r - rgb.r) + std::abs(cand.g - rgb.g) + std::abs(cand.b - rgb.b);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return background ? AnsiBgBits(best & 7, best >= 8) : AnsiFgBits(best & 7, best >= 8);
}

// 一段 SGR 参数串(如 "2;37" 或 "38;5;196")应用到属性上。参数空串按 0
//(整段复位)算;38/48 的扩展色吃掉后续参数;不认得的码忽略——行是本
// 项目自己拼的,出现没约定的码也只丢色不丢字。
//
// bold/dim 单独记账再折算成前景强度位:主题的写法是"1;32"(先加粗后给
// 色),加粗到的那一刻前景还是默认(16 色位表达不了"加亮的默认前景"),
// 得等色号到了再把强度位补上——不然 VT 路的亮绿到直写路褪成暗绿。
void ApplySgrParams(std::uint16_t& attr, bool& bold, bool& dim, std::string_view params) {
    // 逐参数走;38/48 可能各吃 1+2 或 1+5 个后续位。
    std::vector<int> codes;
    {
        std::size_t begin = 0;
        while (begin <= params.size()) {
            const std::size_t sep = params.find(';', begin);
            const std::string_view piece =
                sep == std::string_view::npos ? params.substr(begin) : params.substr(begin, sep - begin);
            codes.push_back(piece.empty() ? 0 : atoi(std::string(piece).c_str()));
            if (sep == std::string_view::npos) {
                break;
            }
            begin = sep + 1;
        }
        if (codes.empty()) {
            codes.push_back(0);
        }
    }
    // 前景强度折算:加粗且未压暗才亮;前景是默认(0)时保默认(亮黑冒充
    //"加亮默认"更离谱)。
    const auto reapply_fg_intensity = [&attr, &bold, &dim]() {
        if ((attr & kNativeAttrFgMask) == 0) {
            return;
        }
        if (dim) {
            attr = static_cast<std::uint16_t>(attr & ~platform::kNativeFgIntensity);  // 压暗优先,亮色也压
        } else if (bold) {
            attr = static_cast<std::uint16_t>(attr | platform::kNativeFgIntensity);  // 加粗补亮,标准色升亮
        }
        // 两者都没置:保留色号自带的那档(90-97 的亮色不被抹)
    };
    for (std::size_t i = 0; i < codes.size(); ++i) {
        const int code = codes[i];
        if (code == 0) {
            attr = 0;  // 复位即"默认属性"记号,落盘时换控制台默认
            bold = false;
            dim = false;
        } else if (code == 1) {
            // 1 与 2 互斥、后者胜:conhost 的 VT 引擎只有一个强度槽,先 faint
            // 后 bold 最终按 bold 渲染。跨序列黏连的 dim("2;37"...未复位..."1;31")
            // 若不清掉,亮红会被压成暗红,与 VT 路画面分叉。
            bold = true;
            dim = false;
            reapply_fg_intensity();
        } else if (code == 2) {
            dim = true;
            bold = false;
            reapply_fg_intensity();
        } else if (code == 22) {
            bold = false;
            dim = false;
            reapply_fg_intensity();
        } else if (code >= 30 && code <= 37) {
            attr = static_cast<std::uint16_t>((attr & ~kNativeAttrFgMask) | AnsiFgBits(code - 30, false));
            reapply_fg_intensity();
        } else if (code >= 90 && code <= 97) {
            attr = static_cast<std::uint16_t>((attr & ~kNativeAttrFgMask) | AnsiFgBits(code - 90, true));
            reapply_fg_intensity();
        } else if (code == 39) {
            attr = static_cast<std::uint16_t>(attr & ~kNativeAttrFgMask);
        } else if (code >= 40 && code <= 47) {
            attr = static_cast<std::uint16_t>((attr & ~kNativeAttrBgMask) | AnsiBgBits(code - 40, false));
        } else if (code >= 100 && code <= 107) {
            attr = static_cast<std::uint16_t>((attr & ~kNativeAttrBgMask) | AnsiBgBits(code - 100, true));
        } else if (code == 49) {
            attr = static_cast<std::uint16_t>(attr & ~kNativeAttrBgMask);
        } else if (code == 38 || code == 48) {
            const bool background = code == 48;
            if (i + 1 >= codes.size()) {
                continue;
            }
            if (codes[i + 1] == 5 && i + 2 < codes.size()) {
                const RgbTriplet rgb = Xterm256Rgb(codes[i + 2]);
                const std::uint16_t bits = NearestNativeBits(rgb, background);
                attr = static_cast<std::uint16_t>(
                    background ? ((attr & ~kNativeAttrBgMask) | bits) : ((attr & ~kNativeAttrFgMask) | bits));
                if (!background) {
                    reapply_fg_intensity();
                }
                i += 2;
            } else if (codes[i + 1] == 2 && i + 4 < codes.size()) {
                const RgbTriplet rgb{codes[i + 2], codes[i + 3], codes[i + 4]};
                const std::uint16_t bits = NearestNativeBits(rgb, background);
                attr = static_cast<std::uint16_t>(
                    background ? ((attr & ~kNativeAttrBgMask) | bits) : ((attr & ~kNativeAttrFgMask) | bits));
                if (!background) {
                    reapply_fg_intensity();
                }
                i += 4;
            } else {
                // 形状不认得:整段忽略(不吃后续位,免得把别的码误当颜色)。
            }
        }
        // 其余码(斜体/下划线/反显等)忽略:footer 行不用,conhost 16 色
        // 属性也表达不了,丢了不影响字符本体。
    }
}

}  // namespace

InlineFrameDiffStats QueueInlineFrameDiff(platform::TerminalBatch& batch,
                                          const InlineFrame* previous,
                                          const InlineFrame& next, int origin_y) {
    InlineFrameDiffStats stats;
    bool cursor_hidden = false;
    const auto hide_cursor = [&]() {
        if (!cursor_hidden) {
            batch.HideCursor();
            cursor_hidden = true;
        }
    };
    const std::size_t old_size = previous == nullptr ? 0 : previous->rows.size();
    stats.compared_rows = (std::max)(old_size, next.rows.size());

    const std::vector<InlineRowChange> changes = ComputeInlineRowChanges(previous, next);
    stats.changed_rows = changes.size();
    for (const InlineRowChange& change : changes) {
        hide_cursor();
        const int y = origin_y + static_cast<int>(change.row);
        if (change.hard_clear) {
            batch.ClearRowHardFrom(change.clear_x, y, change.clear_end - change.clear_x);
        } else {
            batch.ClearRowFrom(change.clear_x, y, change.clear_end - change.clear_x);
        }
        if (change.write_x >= 0) {
            batch.MoveTo(change.write_x, y);
            batch.Write(change.text);
        }
    }

    stats.cursor_changed = previous == nullptr || previous->cursor_x != next.cursor_x ||
                           previous->cursor_row != next.cursor_row;
    stats.emitted = stats.changed_rows > 0 || stats.cursor_changed;
    if (stats.emitted) {
        hide_cursor();
        batch.MoveTo(next.cursor_x, origin_y + next.cursor_row);
        batch.ShowCursor();
    }
    return stats;
}

std::vector<platform::NativeRowCell> BuildNativeRowCells(std::string_view utf8_text, int cell_count) {
    std::vector<platform::NativeRowCell> cells;
    if (cell_count <= 0) {
        return cells;
    }
    cells.reserve(static_cast<std::size_t>(cell_count));
    std::uint16_t attr = 0;  // 默认属性记号
    bool bold = false;
    bool dim = false;
    std::size_t i = 0;
    while (i < utf8_text.size() && static_cast<int>(cells.size()) < cell_count) {
        if (utf8_text[i] == '\x1b' && i + 1 < utf8_text.size() && utf8_text[i + 1] == '[') {
            // CSI 序列整段吃掉:只有 'm' 结尾的动属性,别的(光标/擦除类,
            // 正常不会出现在行文本里)一文不加、直接跳过。
            std::size_t j = i + 2;
            while (j < utf8_text.size()) {
                const unsigned char ch = static_cast<unsigned char>(utf8_text[j++]);
                if (ch >= 0x40U && ch <= 0x7EU) {
                    break;
                }
            }
            if (j <= utf8_text.size() && j > i + 2) {
                const char final_ch = utf8_text[j - 1];
                if (final_ch == 'm') {
                    ApplySgrParams(attr, bold, dim, utf8_text.substr(i + 2, j - 1 - (i + 2)));
                }
            }
            i = j;
            continue;
        }
        // UTF-8 码点解码。
        const unsigned char lead = static_cast<unsigned char>(utf8_text[i]);
        std::size_t len = 1;
        char32_t cp = lead;
        if ((lead & 0xE0U) == 0xC0U) {
            cp = lead & 0x1FU;
            len = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            cp = lead & 0x0FU;
            len = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            cp = lead & 0x07U;
            len = 4;
        }
        for (std::size_t k = 1; k < len && i + k < utf8_text.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8_text[i + k]) & 0x3FU);
        }
        i += len;
        const int width = CharDisplayWidth(cp);
        if (width <= 0) {
            continue;  // 零宽字符(组合附标一类)不占格
        }
        if (static_cast<int>(cells.size()) + width > cell_count) {
            break;  // 放不下整个宽字就截断,不劈半个宽字
        }
        if (width == 2) {
            cells.push_back(platform::NativeRowCell{cp, static_cast<std::uint16_t>(attr | platform::kNativeCellLeading)});
            cells.push_back(platform::NativeRowCell{cp, static_cast<std::uint16_t>(attr | platform::kNativeCellTrailing)});
        } else {
            cells.push_back(platform::NativeRowCell{cp, attr});
        }
    }
    // 尾部补默认属性空格铺满清写宽度——直写整行(新文本 + 残段清空)一发
    // 落盘,这就是"擦行 + 落字"合并成一次 WriteConsoleOutput 的那一步。
    while (static_cast<int>(cells.size()) < cell_count) {
        cells.push_back(platform::NativeRowCell{U' ', 0});
    }
    return cells;
}

bool PaintInlineFrameNativeRows(const InlineFrame* previous, const InlineFrame& next, int origin_y,
                                std::size_t* painted_rows) {
    if (painted_rows != nullptr) {
        *painted_rows = 0;
    }
    std::size_t written = 0;
    for (const InlineRowChange& change : ComputeInlineRowChanges(previous, next)) {
        const int y = origin_y + static_cast<int>(change.row);
        const int clear_count = change.clear_end - change.clear_x;
        if (clear_count <= 0) {
            continue;
        }
        if (change.write_x < 0) {
            // 只清不写:整段默认属性空格直写(hard/soft 之别在直写路里同为
            //"铺默认空格"——footer 行不带背景色,两种清行的视觉结果一致)。
            const std::vector<platform::NativeRowCell> blanks = BuildNativeRowCells(std::string_view(), clear_count);
            if (!platform::WriteNativeRow(change.clear_x, y, blanks.data(), static_cast<int>(blanks.size()))) {
                return false;
            }
            ++written;
            continue;
        }
        // 旧行比新行更靠左的那段先铺空格(合并清写区间的左半边),再从
        // write_x 直写"新文本 + 尾部空格"。
        const int lead_blanks = change.write_x - change.clear_x;
        if (lead_blanks > 0) {
            const std::vector<platform::NativeRowCell> blanks = BuildNativeRowCells(std::string_view(), lead_blanks);
            if (!platform::WriteNativeRow(change.clear_x, y, blanks.data(), static_cast<int>(blanks.size()))) {
                return false;
            }
        }
        const std::vector<platform::NativeRowCell> cells =
            BuildNativeRowCells(change.text, change.clear_end - change.write_x);
        if (!platform::WriteNativeRow(change.write_x, y, cells.data(), static_cast<int>(cells.size()))) {
            return false;
        }
        ++written;
    }
    if (painted_rows != nullptr) {
        *painted_rows = written;
    }
    return true;
}

bool TurnActivityRowChanged(std::string_view old_label, long long old_seconds, bool old_interrupted,
                            std::string_view new_label, long long new_seconds, bool new_interrupted) {
    return old_seconds != new_seconds || old_interrupted != new_interrupted || old_label != new_label;
}

ViewportRevealPlan ComputeViewportReveal(int buffer_height, int viewport_y, int viewport_height, int top_row,
                                         int rows_needed) {
    ViewportRevealPlan plan;
    if (rows_needed <= 0 || buffer_height <= 0) {
        return plan;
    }
    const int window_rows = viewport_height > 0 ? viewport_height : buffer_height;
    int viewport_bottom = viewport_y + window_rows - 1;
    if (viewport_bottom > buffer_height - 1) {
        viewport_bottom = buffer_height - 1;  // 防御:窗口报得比缓冲还长,按缓冲算
    }
    const int needed_bottom = top_row + rows_needed - 1;
    int overflow = needed_bottom - viewport_bottom;
    if (overflow <= 0) {
        return plan;  // 已经在可视区里,一笔都不动
    }
    const int room_below = buffer_height - 1 - viewport_bottom;
    plan.pan_rows = room_below > 0 ? (std::min)(overflow, room_below) : 0;
    plan.scroll_rows = overflow - plan.pan_rows;
    return plan;
}

FooterResizeRecoveryPlan ComputeFooterResizeRecovery(
    int previous_top_row, int previous_input_row, int current_cursor_row,
    const std::vector<int>& previous_row_widths, std::size_t input_row_index,
    int input_cursor_column, int current_width) {
    FooterResizeRecoveryPlan plan;
    plan.top_row = (std::max)(0, previous_top_row);
    plan.rows_to_clear = static_cast<int>(previous_row_widths.size());
    if (current_width <= 0 || previous_row_widths.empty()) {
        return plan;
    }

    // 经典控制台改 buffer 宽时不一定 reflow；光标行未动便说明旧绝对账仍真。
    if (current_cursor_row == previous_input_row) {
        return plan;
    }

    const auto wrapped_rows = [current_width](int display_width) {
        const int cells = (std::max)(0, display_width);
        return (std::max)(1, (cells + current_width - 1) / current_width);
    };
    const std::size_t safe_input_index = (std::min)(input_row_index, previous_row_widths.size());
    int rows_before_input = 0;
    for (std::size_t i = 0; i < safe_input_index; ++i) {
        rows_before_input += wrapped_rows(previous_row_widths[i]);
    }
    const int cursor_wrap_rows = (std::max)(0, input_cursor_column) / current_width;
    plan.top_row = (std::max)(0, current_cursor_row - rows_before_input - cursor_wrap_rows);
    plan.rows_to_clear = 0;
    for (const int width : previous_row_widths) {
        plan.rows_to_clear += wrapped_rows(width);
    }
    plan.cursor_reflowed = true;
    return plan;
}

}  // namespace lubancode::cli
