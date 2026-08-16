// /provider add 向导(向导重排单重写):八步不再顺次写死,改由显式 step enum
// 驱动前进/后退/跳转/取消/确认。步骤序:
//   名字 → 接口格式 → base_url → 密钥来源 → 模型 → 推理档位 → 额外参数 → 确认
// wire 先于 base_url,/v1 提示与最终探测 URL 才算得准。
//
// 每步一个纯校验函数(只回结果,不困在 while(true));IO 走 WizardIO 的
// 注入点(print/read_line/fetch_models/choose + read_event/draw_frame),
// 单测用脚本化事件序列驱动,不碰真实终端与网络。TTY 下 draw_frame 接
// WizardPanel(原地清旧画新),非 TTY 自动退化朴素逐行。
//
// 返回规矩(规格"返回规矩"节)落在状态机里:每步能回、旧值作默认、模型
// 拉取失败页可直达 wire/base_url、改了 wire/base_url/密钥旧模型列表作废
// 重拉、key 页不回显明文、第一步再退弹确认、汇总页可跳回单项修改。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "api/models.hpp"
#include "cli/setup_wizard.hpp"
#include "config/config.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::cli {

struct ProviderWizardOutcome {
    config::ProviderConfig provider;
    bool save_requested = false;  // 最后一问"确认写入?"用户是否同意
};

// 八步的显式次序(见文件头)。顺序就是枚举顺序,别乱动——进度 N/8、后退
// 目标、汇总页跳回全按这个序算。
enum class ProviderWizardStep { Name, Wire, BaseUrl, Auth, Model, Effort, ExtraBody, Confirm };
constexpr std::size_t kProviderWizardStepCount = 8;

// step -> 0 基下标 / 下标 -> step(越界钳到两端,进度与跳转统一走这两个)。
std::size_t ProviderWizardStepIndex(ProviderWizardStep step);
ProviderWizardStep ProviderWizardStepAt(std::size_t index);

// 向导运行态:当前步骤、各字段、字段是否已填妥、模型列表缓存与失效标记。
// 校验函数只回结果;改 wire/base_url/密钥来源的落点负责置 models_valid =
// false,前进到模型步时重拉,不沿用旧缓存。
struct ProviderWizardState {
    ProviderWizardStep step = ProviderWizardStep::Name;
    config::ProviderConfig draft;
    std::vector<config::ProviderConfig> existing;  // 查重名用
    bool name_set = false;
    bool wire_set = false;
    bool base_url_set = false;
    bool auth_set = false;
    bool model_set = false;
    bool effort_set = false;
    bool extra_body_set = false;
    bool models_valid = false;                 // 模型列表缓存可用
    std::vector<api::ModelInfo> models;        // 上次拉到的列表
    std::optional<api::Error> last_fetch_error;  // 最近一次拉模型失败(手填页的
                                                // Back 靠它回到失败页);拉成即清
    // 汇总页跳回单项修改的回程票:从确认页按项号跳回时置 Confirm,该项改完
    // 直接回汇总,后续无关步骤不重走;中途按 Back(自己往前走)即作废。
    std::optional<ProviderWizardStep> return_to;
    std::string last_error;                    // 当前步骤错误;换步/改正后清
};

// name_prefill:命令行已经给的名字(`/provider add 名字`),非空且校验通过
// 就跳过"名字"这一步;校验没过进交互问答(先打一行提示)。
// existing:当前已配的 providers,用来查重名(config::FindProvider)。
//
// 中途 EOF/Ctrl+C/退出确认,返回 std::nullopt——不写盘、不落半截配置。
// 最后一问用户回答 n/N 时返回值有值但 save_requested == false。
std::optional<ProviderWizardOutcome> RunProviderAddWizard(WizardIO& io, const std::string& name_prefill,
                                                          const std::vector<config::ProviderConfig>& existing);

// 带在线目录的入口：先选常见厂家，最后一项仍可走全手填旧向导。选中预设后
// base_url/wire/model/窗口/私有参数全带上，密钥来源与确认复用新向导的
// 同一套导航原语(无退选件,做稳为先)。
std::optional<ProviderWizardOutcome> RunProviderPresetWizard(
    WizardIO& io, const config::ProviderCatalog& catalog, const std::string& name_prefill,
    const std::vector<config::ProviderConfig>& existing);

// 从一段可能带中文/空格的输入里抽一个合法 provider 名字的建议:空白折成
// 短横线,不合规字符丢弃,折叠出的一串作名字。抽不出(全是不合规字符)回
// 空串。只建议,不暗改——名字页报错时捎带展示。
std::string SuggestProviderSlug(const std::string& raw);

// 按 wire 算默认环境变量名:anthropic 一系 ANTHROPIC_AUTH_TOKEN,OpenAI
// 兼容一系 OPENAI_API_KEY。接口格式改了,密钥页的默认值跟着刷新。
std::string DefaultKeyEnvForWire(config::Wire wire);

// base_url 是不是本机/局域网地址(127.0.0.1、localhost、192.168.*、10.*、
// 172.16-31.*):密钥页把光标默认停在"无需鉴权"那一项用。只挪光标,仍须
// 用户按 Enter 明确确认,绝不凭地址暗自判定。
bool IsLocalBaseUrl(const std::string& base_url);

}  // namespace lubancode::cli
