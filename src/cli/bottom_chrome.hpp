// 底栏的一本帧账(0.29.x"导航贴底并整帧去重"):空闲 composer 与流式
// footer 共用的帧描述。规格"整帧重画"一节写死——两条路不得各拼一套行序,
// 布局函数只产行与高度,终端 painter 只管锚点、擦除、落笔。
//
// 帧的行序(自上而下):
//   help_rows(场景按键帮助层,`?` 展开;空 = 没开,垫最顶)
//   activity_rows(Working 活动条)
//   queue_rows(待发队列,composer 上横线之上)
//   上横线 / top_padding 留白 / composer_rows 行输入 / bottom_padding 补空 / 下横线
//   status_rows 行状态栏
//   agent_dock_rows(导航坞,贴底)
//   transient_rows(slash 提示等短命 UI,垫最底)
//
// Composer 合流 P1(终端Composer合流单):idle 与 busy 不再各拼一套输入行。
// 两边都组同一只 BottomChromeModel、调唯一的 BuildBottomChromeLayout 纯函数,
// 输入行拼装、padding、最小正文高度、光标从此只有一份账。painter 与帧账
// 的合流是 P2 的活,这里先把"布局"收拢。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cli/agent_panel.hpp"
#include "cli/line_editor.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

struct BottomChromeFrame {
    std::vector<std::string> help_rows;       // 场景帮助层(0.32.x ? 开合;空 = 没开)
    std::vector<std::string> activity_rows;   // Working 活动条(空闲空)
    std::vector<std::string> queue_rows;      // 待发队列(空队列零行)
    std::vector<std::string> agent_dock_rows; // 导航坞(无子代理零行)
    std::vector<std::string> transient_rows;  // slash 提示等(常态零行)
    // Composer 摘要:草稿全文 + 光标 + 档位 + 占位提示拼成的一串,给指纹
    // 用。合流前指纹只认行数,正文/光标变了指纹不动;合流后"内容变必变"
    // 才真正成立(P1)。
    std::string composer_digest;
    int composer_rows = 1;     // 输入区物理行数(软换行与上下留白算在内)
    int status_rows = 1;       // 状态栏行数
    int rule_rows = 2;         // 上下横线
    int selected_task_id = 0;  // 导航当前选中(0=main,-1=汇总哨兵)
    std::uint64_t revision = 0;  // 帧身份:内容变必变

    // 整帧行数(帮助+活动条+队列+横线+输入+状态+坞+提示)。
    int TotalRows() const {
        return static_cast<int>(help_rows.size()) + static_cast<int>(activity_rows.size()) +
               static_cast<int>(queue_rows.size()) + composer_rows + rule_rows + status_rows +
               static_cast<int>(agent_dock_rows.size()) + static_cast<int>(transient_rows.size());
    }
    // 坞首行相对帧顶的偏移:帮助/队列之后、框与状态栏之下。
    int AgentDockFirstRow() const {
        return static_cast<int>(help_rows.size()) + static_cast<int>(activity_rows.size()) +
               static_cast<int>(queue_rows.size()) + composer_rows + rule_rows + status_rows;
    }
};

// Composer 的档位只作布局/指纹的标签:同源布局后,Idle 与 Busy 的区别
// 应只剩数据(mode、activity、queue 标题),不再剩画法。P3 引入按键策略
// 表时再扩 QueueEdit/ModalSuspended,本单先钉前两档。
enum class ComposerMode {
    Idle,       // 空闲:Enter 开主回合
    BusyQueue,  // 忙时:Enter 入 SteeringQueue
};

// 主 composer 默认只占一行正文,紧贴上下横线。多行与软换行出现时再
// 自然长高。Idle 与 Busy 共用这一对常量——改高度只许改这里,两条路
// 同拍生效。
inline constexpr int kComposerTopPaddingRows = 0;
inline constexpr int kComposerMinBodyRows = 1;

// 输入区的完整视图模型:布局只认这一份,不认 echo 摘要、不认行数整数。
// editor 带全部逻辑行与光标;placeholder 只在主草稿真空时显示(空串 =
// 不显示,空闲路如今便是空串);prompt 由布局自算显示宽,调用方不必
// 另传 prompt_end_col。
struct ComposerViewModel {
    RenderState editor;
    std::string prompt;                            // 首行提示符("> ")
    std::string placeholder;                       // 草稿真空时的占位提示
    ComposerMode mode = ComposerMode::Idle;
    ConfirmMode confirm_mode = ConfirmMode::Confirm;
    int min_body_rows = kComposerMinBodyRows;
    int top_padding_rows = kComposerTopPaddingRows;
};

// 整块底栏的输入模型。status_rows 由调用方拼好(width 感知的文案在两条
// 路里本来就不同:空闲是纯状态行,忙时尾部多一段 Esc 打断提示),布局
// 只负责摆位与截断。framed=false 是无框单行读取(向导/确认提示)的退化
// 形态:不画横线、不留白、不摆状态行。
struct BottomChromeModel {
    std::vector<std::string> help_rows;        // 场景帮助层(空 = 没开,垫帧最顶)
    std::vector<std::string> activity_rows;    // Working 活动条(空闲空)
    std::vector<std::string> queue_rows;       // 待发队列(空队列零行)
    ComposerViewModel composer;
    std::vector<std::string> status_rows;      // 状态栏(调用方拼好的整行)
    std::vector<std::string> agent_dock_rows;  // 导航坞(无子代理零行)
    // 坞行的监督色辅助(监督器单 P1-1):与 agent_dock_rows 按位对齐,可短
    //(不足位按 Normal);行文本保持纯文本,颜色由布局在这包 ANSI。
    std::vector<AgentHealthTint> agent_dock_tints;
    std::vector<std::string> transient_rows;   // slash 提示等(常态零行)
    std::string rule_tag;                      // 上横线右端短标签(查看态)
    int selected_task_id = 0;                  // 导航当前选中(0=main)
    bool framed = true;                        // 是否带横线框(见上)
};

// ---------------------------------------------------------------------------
// 帮助层开合状态机(纯,`?` 键位帮助只能展开不能收起单):终端层各处只报
// 事件,下一状态全在这查——谁也不许多写一份 if。事件语义:
//   TogglePressed    空 composer 按下 help.show 和弦(非空时那枚键是普通
//                    字符,事件压根不发生);
//   EscapePressed    Esc(帮助层开着时优先收帮助,不给编辑器/面板);
//   DraftFilled      草稿从空变非空(打字/粘贴/取回),场景换了;
//   SceneChanged     底栏让位或场景切换(搜索开/队列取回/外部编辑器/转录
//                    导航/查看态切换……),帮助层跟着底栏一起走。
// ---------------------------------------------------------------------------
enum class HelpOverlayEvent {
    TogglePressed,
    EscapePressed,
    DraftFilled,
    SceneChanged,
};

bool HelpOverlayNext(bool visible, HelpOverlayEvent event);

// BuildBottomChromeLayout 的产物:可直接落笔的整帧 + 行数账 + 光标。
// cursor_row 相对帧顶;composer_first_row 是首个输入物理行在帧里的下标。
// painted_row_widths 是每行"纯文本显示宽"(剥掉 ANSI 后按显示宽算),
// footer 的 resize 旧帧追踪(ComputeFooterResizeRecovery)靠它反推 reflow。
// dropped_optional_rows:高度预算钳制(见 BuildBottomChromeLayout 的
// height_budget)舍掉了多少行可选内容(活动条/队列/坞/提示)。输入区本体
// 不在可舍之列,不算进这个数。
struct BottomChromeLayout {
    InlineFrame frame;         // 逐行内容(x/清宽/文本)与帧内光标
    BottomChromeFrame chrome;  // 行数账 + 指纹(含 composer 摘要)
    int composer_first_row = 0;
    int composer_row_count = 0;  // 输入物理行数(不含上下留白)
    int cursor_x = 0;
    int cursor_row = 0;
    std::vector<int> painted_row_widths;
    std::uint64_t revision = 0;
    int dropped_optional_rows = 0;
};

// 全程序唯一的 Composer 布局入口:软换行(LayoutComposerRows)、首行
// prompt 与续行两格缩进、CJK/emoji 显示宽、上下留白与最小正文高度、
// 上下横线与 rule tag、queue/activity/status/dock/transient 的固定行序、
// placeholder、真实物理光标、行数账与指纹,全在这一个纯函数里。
//
// height_budget(终端画面隔网单·战术二,0 = 不限):整帧最多占的行数
// (调用方传可视窗口高)。"输入行必画得下"在这里立成硬约束——超预算时
// 按 transient(提示)→ dock(坞)→ queue(队列)→ activity(活动条)
// 的次序舍可选行;可选行全舍了还不够(多行输入比窗还高),再把 composer
// 的物理行围光标开窗,横线/状态行跟着让位。与 LayoutAgentDock 的矮屏
// 预算、ClampAnsiRowToWidth 的窄窗截断同族,都是"布局层兜底,不让画面
// 撑爆终端"的一环,别另起炉灶。
BottomChromeLayout BuildBottomChromeLayout(const BottomChromeModel& model, const Theme& theme,
                                            int terminal_width, int height_budget = 0);

// 一根框线(带主题淡色;plain 主题 theme.stats/reset 都是空串,自动退化成
// 无色 '-' 线)。从 console_input.cpp 挪来:布局函数要画横线,不能反过来
// 依赖终端层。max_width 传终端宽自身,min(width-1, width) 恒等于 width-1,
// 框线满终端宽。
std::string BoxRuleLine(const Theme& theme, int console_width);

// 帧指纹:行内容 + 选择 + 高度拼成一串,给"变了才重画"的比较用;revision
// 由同内容哈希而来(内容同则 revision 同,不引入额外状态)。
std::string BottomChromeFingerprint(const BottomChromeFrame& frame);
std::uint64_t BottomChromeRevision(const BottomChromeFrame& frame);

}  // namespace lubancode::cli
