// /provider add 向导(向导重排单):ProviderWizardState + 显式 step enum 驱动
// 的状态机。八步见 provider_wizard.hpp 文件头。校验函数只回结果,不困在
// while(true);前进/后退/跳转/取消/确认全在主循环里按 StepResult 挪步。
// TTY 下 draw_frame 接 WizardPanel(原地清旧画新),非 TTY 自动退化朴素
// 逐行——两条路吃同一份逻辑,单测只注入脚本化事件序列。

#include "cli/provider_wizard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "api/models.hpp"
#include "cli/i18n.hpp"
#include "config/config.hpp"
#include "platform/paths.hpp"

namespace lubancode::cli {

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string TrimCopy(const std::string& s) {
    return WizardTrim(s);
}

// 探测地址展示行:模型列表将从 {url} 读取。
std::string ProbeUrlLine(config::Wire wire, const std::string& base_url) {
    return trf("provider_wizard.base_url.probe", api::ModelsUrl(wire, base_url));
}

// 拉模型用的 key:按鉴权三态解。none/缺 env 给空串(ListModels 对空 key
// 彻底省鉴权头),inline/已取到给值。
std::string FetchKeyForDraft(const config::ProviderConfig& draft) {
    const config::ProviderAuthResolution auth = config::ResolveProviderAuth(draft);
    return auth.status == config::ProviderAuthResolution::Status::Ready ? *auth.key : std::string();
}

// api::Error 的短摘要:只露状态码/类别与一小段正文,不把底层响应整坨糊到
// 面板上(调试日志另查,不污染对话)。
std::string ShortModelError(const api::Error& error) {
    std::string summary;
    if (error.kind == api::ErrorKind::HttpStatus) {
        summary = "HTTP " + std::to_string(error.http_status);
    } else if (error.kind == api::ErrorKind::Network) {
        summary = tr("provider_wizard.model.err_network");
    } else {
        summary = tr("provider_wizard.model.err_other");
    }
    std::string snippet = error.message;
    // 只留单行、掐头去尾空白,超长截断。
    const std::size_t newline = snippet.find('\n');
    if (newline != std::string::npos) {
        snippet = snippet.substr(0, newline);
    }
    snippet = TrimCopy(snippet);
    if (snippet.size() > 80) {
        snippet = snippet.substr(0, 80) + "...";
    }
    if (!snippet.empty()) {
        summary += " " + snippet;
    }
    return summary;
}

// 按状态码给一条诊断引路(只引路,不把猜测说成定论)。
std::string FetchHintForStatus(const api::Error& error) {
    if (error.kind != api::ErrorKind::HttpStatus) {
        return tr("provider_wizard.model.fetch_network_hint");
    }
    if (error.http_status == 404) {
        return tr("provider_wizard.model.fetch_404_hint");
    }
    if (error.http_status == 401 || error.http_status == 403) {
        return tr("provider_wizard.model.fetch_401_hint");
    }
    return tr("provider_wizard.model.fetch_other_hint");
}

// 汇总页/确认用的鉴权展示行。绝不回显明文 key——inline 只露掩码。
std::string AuthDisplayLine(const config::ProviderConfig& draft) {
    switch (draft.auth) {
        case config::ProviderAuthMode::None:
            return tr("cmd.provider.auth_none");
        case config::ProviderAuthMode::Env:
            return draft.api_key.empty() ? trf("provider_wizard.auth.summary_env", draft.key_env)
                                         : trf("provider_wizard.auth.summary_inline",
                                               config::MaskApiKey(draft.api_key));
        case config::ProviderAuthMode::Inline:
            return trf("provider_wizard.auth.summary_inline", config::MaskApiKey(draft.api_key));
    }
    return std::string();
}

// 面板标题:add 与 edit 各一顶帽子,一眼分清这次进来是添新的还是改旧的。
std::string WizardTitle(const ProviderWizardState& state) {
    return tr(state.edit_mode ? "provider_wizard.edit.title" : "provider_wizard.title");
}

// ---------------------------------------------------------------------------
// 步骤结果:主循环只认这个,各步 handler 不直接改 state.step。
// ---------------------------------------------------------------------------

struct StepResult {
    enum class Action { Stay, Goto, Cancel, Save, NoSave };
    Action action = Action::Stay;
    ProviderWizardStep target = ProviderWizardStep::Name;  // Goto 时有效
};

StepResult Stay() { return StepResult{}; }
StepResult Goto(ProviderWizardStep target) { return StepResult{StepResult::Action::Goto, target}; }
StepResult Cancel() { return StepResult{StepResult::Action::Cancel, ProviderWizardStep::Name}; }
StepResult Save() { return StepResult{StepResult::Action::Save, ProviderWizardStep::Name}; }
StepResult NoSave() { return StepResult{StepResult::Action::NoSave, ProviderWizardStep::Name}; }

// 一次编辑成功后的去向:从汇总页跳回来的(有回程票)直接回汇总,后续无关
// 步骤不重走;正常前进走 forward。往回走的 Back 由主循环统一撕票。
StepResult AfterEdit(ProviderWizardState& state, ProviderWizardStep forward) {
    if (state.return_to.has_value()) {
        const ProviderWizardStep back = *state.return_to;
        state.return_to.reset();
        return Goto(back);
    }
    return Goto(forward);
}

// ---------------------------------------------------------------------------
// 读取助手:文本步骤与选择步骤各一个,都先画帧再读。
// ---------------------------------------------------------------------------

WizardInputEvent ReadTextEvent(WizardIO& io, const WizardFrame& frame) {
    DrawWizardFrame(io, frame);
    return ReadWizardEvent(io);
}

// 选择步骤的读取。返回结构:index 有值 = 选中;没值时 nav 给取消语义
// (Back/Cancelled/Eof)。
struct ChoiceNav {
    std::optional<std::size_t> index;
    WizardInputEvent::Kind nav = WizardInputEvent::Kind::Submitted;
};

ChoiceNav ReadChoiceNav(WizardIO& io, const WizardFrame& frame, const std::vector<WizardChoiceItem>& items,
                        std::size_t default_index) {
    DrawWizardFrame(io, frame);
    if (io.interactive && io.choose) {
        WizardInputEvent::Kind cancel_kind = WizardInputEvent::Kind::Eof;
        auto index = io.choose(items, default_index, frame.footer, &cancel_kind);
        if (index.has_value()) {
            return ChoiceNav{index, WizardInputEvent::Kind::Submitted};
        }
        return ChoiceNav{std::nullopt, cancel_kind};
    }
    // 编号回落:ReadChoice 只在 EOF 时返回 nullopt——朴素路上没有 Esc。
    const auto index = ReadChoice(io, items, default_index, frame.footer);
    if (index.has_value()) {
        return ChoiceNav{index, WizardInputEvent::Kind::Submitted};
    }
    return ChoiceNav{std::nullopt, WizardInputEvent::Kind::Eof};
}

// ---------------------------------------------------------------------------
// 第一步再退的"退出向导?"确认。默认不写盘:空串/n/N 都算不退,y/Y 才退。
// ---------------------------------------------------------------------------

StepResult ConfirmExit(WizardIO& io, const ProviderWizardState& state) {
    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.body = {tr("provider_wizard.exit_confirm.body")};
    frame.prompt = tr("provider_wizard.exit_confirm.prompt");
    frame.footer = tr("provider_wizard.footer.back");
    while (true) {
        const WizardInputEvent event = ReadTextEvent(io, frame);
        switch (event.kind) {
            case WizardInputEvent::Kind::Cancelled:
            case WizardInputEvent::Kind::Eof:
                return Cancel();
            case WizardInputEvent::Kind::Back:
            case WizardInputEvent::Kind::Submitted:
                if (event.kind == WizardInputEvent::Kind::Submitted &&
                    (event.text == "y" || event.text == "Y")) {
                    return Cancel();
                }
                return Stay();  // 不退,留在当前步
        }
    }
}

// ---------------------------------------------------------------------------
// 各步 handler。规矩:校验函数只回结果;改正或换步清 state.last_error;
// 改 wire/base_url/密钥来源后置 models_valid = false。
// ---------------------------------------------------------------------------

StepResult RunNameStep(WizardIO& io, ProviderWizardState& state) {
    if (state.edit_mode) {
        // 编辑模式名字锁死:回车/Back 都回汇总(汇总才是 edit 的家),敲了
        // 新名字就明说"不支持改名",不留任何悄悄改名的口子。
        WizardFrame frame;
        frame.title = WizardTitle(state);
        frame.body = {trf("provider_wizard.edit.name_locked", state.draft.name),
                      tr("provider_wizard.edit.no_rename")};
        frame.error = state.last_error;
        frame.prompt = tr("provider_wizard.edit.name_prompt");
        frame.footer = tr("provider_wizard.footer.back");
        const WizardInputEvent event = ReadTextEvent(io, frame);
        if (event.kind == WizardInputEvent::Kind::Back) {
            return Goto(ProviderWizardStep::Confirm);
        }
        if (event.kind != WizardInputEvent::Kind::Submitted) {
            return Cancel();
        }
        if (!event.text.empty() && event.text != state.draft.name) {
            state.last_error = tr("provider_wizard.edit.no_rename");
            return Stay();
        }
        state.last_error.clear();
        return Goto(ProviderWizardStep::Confirm);
    }
    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.progress = trf("provider_wizard.progress", 1, kProviderWizardStepCount);
    frame.body = {tr("provider_wizard.name.hint")};
    if (state.name_set) {
        frame.body.push_back(trf("provider_wizard.current_value", state.draft.name));
    }
    frame.error = state.last_error;
    frame.prompt = tr("provider_wizard.name.prompt");
    frame.footer = tr("provider_wizard.footer.first");

    const WizardInputEvent event = ReadTextEvent(io, frame);
    if (event.kind == WizardInputEvent::Kind::Back) {
        return ConfirmExit(io, state);
    }
    if (event.kind != WizardInputEvent::Kind::Submitted) {
        return Cancel();
    }
    if (event.text.empty()) {
        state.last_error = tr("provider_wizard.name.empty");
        return Stay();
    }
    const auto valid = config::ValidateProviderName(event.text, state.existing);
    if (!valid.has_value()) {
        std::string error = valid.error();
        const std::string slug = SuggestProviderSlug(event.text);
        if (!slug.empty() && slug != event.text) {
            error += " " + trf("provider_wizard.name.slug_hint", slug);
        }
        state.last_error = error;
        return Stay();
    }
    state.draft.name = event.text;
    state.name_set = true;
    state.last_error.clear();
    return AfterEdit(state, ProviderWizardStep::Wire);
}

StepResult RunWireStep(WizardIO& io, ProviderWizardState& state) {
    std::vector<WizardChoiceItem> items = {
        {tr("provider_wizard.wire.opt1"), tr("provider_wizard.wire.desc1")},
        {tr("provider_wizard.wire.opt2"), tr("provider_wizard.wire.desc2")},
        {tr("provider_wizard.wire.opt3"), tr("provider_wizard.wire.desc3")},
    };
    std::size_t default_index = 0;
    if (state.wire_set) {
        default_index = state.draft.wire == config::Wire::Responses
                            ? 1
                            : (state.draft.wire == config::Wire::ChatCompletions ? 2 : 0);
    }

    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.progress = trf("provider_wizard.progress", 2, kProviderWizardStepCount);
    frame.body = {tr("provider_wizard.wire.hint")};
    if (state.wire_set) {
        frame.body.push_back(trf("provider_wizard.current_value",
                                 config::ProviderWireName(state.draft.wire)));
    }
    frame.error = state.last_error;
    frame.footer = tr("provider_wizard.footer.back");

    const ChoiceNav choice = ReadChoiceNav(io, frame, items, default_index);
    if (!choice.index.has_value()) {
        if (choice.nav == WizardInputEvent::Kind::Back) {
            return Goto(ProviderWizardStep::Name);
        }
        return Cancel();
    }
    const config::Wire wire =
        *choice.index == 1 ? config::Wire::Responses
                           : (*choice.index == 2 ? config::Wire::ChatCompletions : config::Wire::Anthropic);
    if (state.wire_set && wire != state.draft.wire) {
        state.models_valid = false;  // 探测路径变了,旧列表作废
    }
    state.draft.wire = wire;
    state.wire_set = true;
    state.last_error.clear();
    return AfterEdit(state, ProviderWizardStep::BaseUrl);
}

// base_url 步:校验(剥尾斜杠 + scheme)/v1 建议(只建议,不暗改)。
StepResult RunBaseUrlStep(WizardIO& io, ProviderWizardState& state) {
    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.progress = trf("provider_wizard.progress", 3, kProviderWizardStepCount);
    frame.body = {tr("provider_wizard.base_url.hint")};
    // 接口格式已定,展示最终探测地址——少没少 /v1 一眼瞧得出来。
    frame.body.push_back(ProbeUrlLine(state.draft.wire, state.base_url_set
                                                             ? state.draft.base_url
                                                             : "http://127.0.0.1:8000"));
    if (state.base_url_set) {
        frame.body.push_back(trf("provider_wizard.current_value", state.draft.base_url));
    }
    frame.error = state.last_error;
    frame.prompt = tr("provider_wizard.base_url.prompt");
    frame.footer = tr("provider_wizard.footer.back");

    const WizardInputEvent event = ReadTextEvent(io, frame);
    if (event.kind == WizardInputEvent::Kind::Back) {
        return Goto(ProviderWizardStep::Wire);
    }
    if (event.kind != WizardInputEvent::Kind::Submitted) {
        return Cancel();
    }
    if (event.text.empty()) {
        if (state.base_url_set) {
            return AfterEdit(state, ProviderWizardStep::Auth);  // 回车确认旧值
        }
        state.last_error = tr("provider_wizard.base_url.empty");
        return Stay();
    }
    const std::string url = StripTrailingSlashes(event.text);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        state.last_error = tr("provider_wizard.base_url.bad_scheme");
        return Stay();
    }
    // OpenAI 兼容格式没带 /v1:当场提示"这个服务通常还要 /v1",给采用与否
    // 两个选项。只建议,不暗改——有些服务确实不用这层路径。
    std::string chosen = url;
    if (state.draft.wire != config::Wire::Anthropic && url.find("/v1") == std::string::npos) {
        const std::string with_v1 = url + "/v1";
        WizardFrame offer;
        offer.title = WizardTitle(state);
        offer.progress = trf("provider_wizard.progress", 3, kProviderWizardStepCount);
        offer.body = {trf("provider_wizard.base_url.v1_offer", with_v1)};
        offer.footer = tr("provider_wizard.footer.back");
        const std::vector<WizardChoiceItem> offer_items = {
            {trf("provider_wizard.base_url.v1_opt_use", with_v1), ProbeUrlLine(state.draft.wire, with_v1)},
            {trf("provider_wizard.base_url.v1_opt_keep", url), ProbeUrlLine(state.draft.wire, url)},
        };
        const ChoiceNav pick = ReadChoiceNav(io, offer, offer_items, 0);
        if (!pick.index.has_value()) {
            if (pick.nav == WizardInputEvent::Kind::Back) {
                return Stay();  // 回到 base_url 输入(上一步是 wire,但这里留在本步重填)
            }
            return Cancel();
        }
        chosen = *pick.index == 0 ? with_v1 : url;
    }
    if (state.base_url_set && chosen != state.draft.base_url) {
        state.models_valid = false;  // 地址变了,旧列表作废
    }
    state.draft.base_url = chosen;
    state.base_url_set = true;
    state.last_error.clear();
    return AfterEdit(state, ProviderWizardStep::Auth);
}

// ---------------------------------------------------------------------------
// 密钥来源步(第 4/8 步):三项明白的选择——无需鉴权 / 环境变量 / 贴 key。
// 子输入(变量名、key)的 Back 回到选择页,不再往 wire 倒退一层。
// ---------------------------------------------------------------------------

StepResult RunAuthStep(WizardIO& io, ProviderWizardState& state) {
    // env 子页的默认变量名:key_env 是显式给的(edit 预填、preset 带的、
    // env 子页敲过/确认过)就回车保留,不能悄悄复位;否则按 wire 现算默认
    // ——add 起手垫的那个不算数,wire 改了默认值得跟着换。
    const std::string default_env =
        state.draft.auth == config::ProviderAuthMode::Env && state.key_env_explicit &&
                !state.draft.key_env.empty()
            ? state.draft.key_env
            : DefaultKeyEnvForWire(state.draft.wire);
    while (true) {
        std::vector<WizardChoiceItem> items = {
            {tr("provider_wizard.auth.opt_none"), tr("provider_wizard.auth.desc_none")},
            {tr("provider_wizard.auth.opt_env"), trf("provider_wizard.auth.desc_env", default_env)},
            {tr("provider_wizard.auth.opt_inline"), tr("provider_wizard.auth.desc_inline")},
        };
        std::size_t default_index = 1;
        if (state.auth_set) {
            default_index = state.draft.auth == config::ProviderAuthMode::None
                                ? 0
                                : (state.draft.auth == config::ProviderAuthMode::Inline ? 2 : 1);
        } else if (IsLocalBaseUrl(state.draft.base_url)) {
            // 本机/局域网地址:光标默认停在"无需鉴权"——仍须用户按 Enter
            // 明确确认,不凭地址暗自判定。
            default_index = 0;
        }

        WizardFrame frame;
        frame.title = WizardTitle(state);
        frame.progress = trf("provider_wizard.progress", 4, kProviderWizardStepCount);
        frame.body = {tr("provider_wizard.auth.hint")};
        if (state.auth_set) {
            // 回到这一步不回显明文 key:只写"无需鉴权 / 读取环境变量 XXX /
            // 已设置明文密钥"。
            frame.body.push_back(trf("provider_wizard.current_value", AuthDisplayLine(state.draft)));
        }
        frame.error = state.last_error;
        frame.footer = tr("provider_wizard.footer.back");

        const ChoiceNav choice = ReadChoiceNav(io, frame, items, default_index);
        if (!choice.index.has_value()) {
            if (choice.nav == WizardInputEvent::Kind::Back) {
                return Goto(ProviderWizardStep::BaseUrl);
            }
            return Cancel();
        }
        state.last_error.clear();
        if (*choice.index == 0) {
            if (!(state.auth_set && state.draft.auth == config::ProviderAuthMode::None)) {
                state.models_valid = false;  // 鉴权来源变了,旧列表作废
            }
            state.draft.auth = config::ProviderAuthMode::None;
            state.auth_set = true;
            return AfterEdit(state, ProviderWizardStep::Model);
        }
        if (*choice.index == 1) {
            // 环境变量名:默认值按接口格式算;回车确认默认。
            WizardFrame env_frame;
            env_frame.title = WizardTitle(state);
            env_frame.progress = trf("provider_wizard.progress", 4, kProviderWizardStepCount);
            env_frame.body = {trf("provider_wizard.auth.env.prompt", default_env)};
            const std::optional<std::string> current = platform::GetEnvVar(default_env.c_str());
            env_frame.body.push_back(current.has_value() && !current->empty()
                                         ? trf("provider_wizard.auth.env.note_set", default_env)
                                         : trf("provider_wizard.auth.env.note_unset", default_env));
            env_frame.error = state.last_error;
            env_frame.prompt = tr("provider_wizard.auth.env.input");
            env_frame.footer = tr("provider_wizard.footer.back");
            const WizardInputEvent env_event = ReadTextEvent(io, env_frame);
            if (env_event.kind == WizardInputEvent::Kind::Back) {
                continue;  // 回到选择页
            }
            if (env_event.kind != WizardInputEvent::Kind::Submitted) {
                return Cancel();
            }
            const std::string env_name = env_event.text.empty() ? default_env : env_event.text;
            if (!(state.auth_set && state.draft.auth == config::ProviderAuthMode::Env &&
                  state.draft.key_env == env_name)) {
                state.models_valid = false;
            }
            state.draft.auth = config::ProviderAuthMode::Env;
            state.draft.key_env = env_name;
            state.key_env_explicit = true;  // 敲过或回车确认过,往后按它当默认
            state.auth_set = true;
            return AfterEdit(state, ProviderWizardStep::Model);
        }
        // 贴入明文 key。回到这一步不回显;回车 = 保留已设 key。
        WizardFrame key_frame;
        key_frame.title = WizardTitle(state);
        key_frame.progress = trf("provider_wizard.progress", 4, kProviderWizardStepCount);
        key_frame.body = {tr("provider_wizard.auth.inline.hint")};
        if (!state.draft.api_key.empty()) {
            key_frame.body.push_back(trf("provider_wizard.auth.inline.keep",
                                         config::MaskApiKey(state.draft.api_key)));
        }
        key_frame.error = state.last_error;
        key_frame.prompt = tr("provider_wizard.auth.inline.input");
        key_frame.footer = tr("provider_wizard.footer.back");
        const WizardInputEvent key_event = ReadTextEvent(io, key_frame);
        if (key_event.kind == WizardInputEvent::Kind::Back) {
            continue;  // 回到选择页
        }
        if (key_event.kind != WizardInputEvent::Kind::Submitted) {
            return Cancel();
        }
        if (key_event.text.empty() && state.draft.api_key.empty()) {
            state.last_error = tr("provider_wizard.auth.inline.empty");
            continue;
        }
        if (!key_event.text.empty()) {
            if (!(state.auth_set && state.draft.auth == config::ProviderAuthMode::Inline)) {
                state.models_valid = false;
            }
            state.draft.api_key = key_event.text;
        }
        state.draft.auth = config::ProviderAuthMode::Inline;
        state.auth_set = true;
        return AfterEdit(state, ProviderWizardStep::Model);
    }
}

// ---------------------------------------------------------------------------
// 模型步(第 5/8 步):手填或拉列表;拉取失败页给四/五条去路。
// ---------------------------------------------------------------------------

// 拉列表并选择(缓存作废就重拉,不沿用上回)。失败落失败页,空列表落手填。
StepResult TryFetchAndPick(WizardIO& io, ProviderWizardState& state);

// 模型拉取失败页:手动输入 / [加上 /v1 后重试] / 返回检查接口格式 /
// 返回检查 base_url / 重试。
StepResult RunModelFetchFailed(WizardIO& io, ProviderWizardState& state, const api::Error& error);

// 手填子路:空串报错留在本步;从失败页进来的(最近一次拉取失败还在)Back
// 回失败页,否则回上一步(密钥来源)。
StepResult RunManualModelInput(WizardIO& io, ProviderWizardState& state, const std::string& fetch_summary) {
    while (true) {
        WizardFrame frame;
        frame.title = WizardTitle(state);
        frame.progress = trf("provider_wizard.progress", 5, kProviderWizardStepCount);
        frame.body = {tr("provider_wizard.model.manual_hint")};
        if (!fetch_summary.empty()) {
            frame.body.push_back(fetch_summary);
        }
        if (state.model_set) {
            frame.body.push_back(trf("provider_wizard.current_value", state.draft.model));
        }
        frame.error = state.last_error;
        frame.prompt = tr("provider_wizard.model.prompt");
        frame.footer = tr("provider_wizard.footer.back");
        const WizardInputEvent event = ReadTextEvent(io, frame);
        if (event.kind == WizardInputEvent::Kind::Back) {
            if (state.last_fetch_error.has_value()) {
                return RunModelFetchFailed(io, state, *state.last_fetch_error);
            }
            return Goto(ProviderWizardStep::Auth);
        }
        if (event.kind != WizardInputEvent::Kind::Submitted) {
            return Cancel();
        }
        if (event.text.empty()) {
            state.last_error = tr("provider_wizard.model.empty");
            continue;
        }
        state.draft.model = event.text;
        state.model_set = true;
        state.last_error.clear();
        return AfterEdit(state, ProviderWizardStep::Effort);
    }
}

StepResult RunModelStep(WizardIO& io, ProviderWizardState& state) {
    // 第一问:回车拉列表,或直接输入模型名。
    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.progress = trf("provider_wizard.progress", 5, kProviderWizardStepCount);
    if (state.edit_mode) {
        // 编辑模式回车=保留当前模型,不发网络请求——"只改 base_url"这种
        // 常见活不该被一回车拽去拉列表。要换模型就手敲新名字。
        frame.body = {tr("provider_wizard.edit.model.hint")};
    } else {
        frame.body = {trf("provider_wizard.model.probe", api::ModelsUrl(state.draft.wire, state.draft.base_url),
                          config::ProviderWireName(state.draft.wire)),
                      tr("provider_wizard.model.hint")};
        // 环境变量没设,拉模型前先说清,不等请求失败才叫用户猜。
        if (state.draft.auth == config::ProviderAuthMode::Env && state.draft.api_key.empty()) {
            const std::optional<std::string> value = platform::GetEnvVar(state.draft.key_env.c_str());
            if (!value.has_value() || value->empty()) {
                frame.body.push_back(trf("provider_wizard.auth.env.note_unset", state.draft.key_env));
            }
        }
    }
    if (state.model_set) {
        frame.body.push_back(trf("provider_wizard.current_value", state.draft.model));
    }
    frame.error = state.last_error;
    frame.prompt = tr("provider_wizard.model.prompt");
    frame.footer = tr("provider_wizard.footer.back");

    const WizardInputEvent event = ReadTextEvent(io, frame);
    if (event.kind == WizardInputEvent::Kind::Back) {
        return Goto(ProviderWizardStep::Auth);
    }
    if (event.kind != WizardInputEvent::Kind::Submitted) {
        return Cancel();
    }
    if (!event.text.empty()) {
        state.draft.model = event.text;
        state.model_set = true;
        state.last_error.clear();
        return AfterEdit(state, ProviderWizardStep::Effort);
    }
    if (state.edit_mode && state.model_set) {
        state.last_error.clear();
        return AfterEdit(state, ProviderWizardStep::Effort);  // 回车保留旧模型
    }
    // 回车:拉列表(或沿用仍有效的缓存)。
    return TryFetchAndPick(io, state);
}

StepResult TryFetchAndPick(WizardIO& io, ProviderWizardState& state) {
    if (!state.models_valid) {
        const auto fetched =
            io.fetch_models(state.draft.wire, state.draft.base_url, FetchKeyForDraft(state.draft));
        if (!fetched.has_value()) {
            // 失败页:错误要短、贴着当前字段;去路摆明。
            state.last_fetch_error = fetched.error();
            return RunModelFetchFailed(io, state, fetched.error());
        }
        state.models = *fetched;
        state.models_valid = true;
        state.last_fetch_error.reset();
    }
    if (state.models.empty()) {
        state.last_error = tr("provider_wizard.model.list_empty");
        return RunManualModelInput(io, state, std::string());
    }

    std::vector<WizardChoiceItem> items;
    items.reserve(state.models.size());
    for (const auto& m : state.models) {
        items.push_back({m.display_name.empty() ? m.id : m.display_name, m.id});
    }
    std::size_t default_index = 0;
    if (state.model_set) {
        for (std::size_t i = 0; i < state.models.size(); ++i) {
            if (state.models[i].id == state.draft.model) {
                default_index = i;
                break;
            }
        }
    }
    WizardFrame list_frame;
    list_frame.title = WizardTitle(state);
    list_frame.progress = trf("provider_wizard.progress", 5, kProviderWizardStepCount);
    list_frame.body = {trf("provider_wizard.model.probe", api::ModelsUrl(state.draft.wire, state.draft.base_url),
                           config::ProviderWireName(state.draft.wire))};
    list_frame.footer = tr("provider_wizard.footer.back");
    const ChoiceNav choice = ReadChoiceNav(io, list_frame, items, default_index);
    if (!choice.index.has_value()) {
        if (choice.nav == WizardInputEvent::Kind::Back) {
            return Goto(ProviderWizardStep::Auth);
        }
        return Cancel();
    }
    state.draft.model = state.models[*choice.index].id;
    state.model_set = true;
    state.last_error.clear();
    return AfterEdit(state, ProviderWizardStep::Effort);
}

StepResult RunModelFetchFailed(WizardIO& io, ProviderWizardState& state, const api::Error& error) {
    const std::string summary = trf("provider_wizard.model.fetch_failed", ShortModelError(error));
    // OpenAI 兼容地址没带 /v1 又吃了 404:给"加上 /v1 后重试"这一项,把改后
    // 的完整探测 URL 摆出来,用户确认后才改值。
    const bool offer_add_v1 = error.kind == api::ErrorKind::HttpStatus && error.http_status == 404 &&
                              state.draft.wire != config::Wire::Anthropic &&
                              state.draft.base_url.find("/v1") == std::string::npos;

    while (true) {
        std::vector<WizardChoiceItem> items;
        if (offer_add_v1) {
            items.push_back(
                {trf("provider_wizard.model.opt_add_v1", state.draft.base_url + "/v1/models"), {}});
        }
        items.push_back({tr("provider_wizard.model.opt_manual"), {}});
        items.push_back({tr("provider_wizard.model.opt_back_wire"), {}});
        items.push_back({tr("provider_wizard.model.opt_back_url"), {}});
        items.push_back({tr("provider_wizard.model.opt_retry"), {}});

        WizardFrame frame;
        frame.title = WizardTitle(state);
        frame.progress = trf("provider_wizard.progress", 5, kProviderWizardStepCount);
        frame.body = {summary, FetchHintForStatus(error)};
        frame.footer = tr("provider_wizard.footer.back");

        const ChoiceNav choice = ReadChoiceNav(io, frame, items, 0);
        if (!choice.index.has_value()) {
            if (choice.nav == WizardInputEvent::Kind::Back) {
                return Goto(ProviderWizardStep::Auth);
            }
            return Cancel();
        }
        std::size_t index = *choice.index;
        if (offer_add_v1) {
            if (index == 0) {
                // 用户确认后才改值:地址补上 /v1,立刻重拉(完整探测 URL 已
                // 摆在选项里)。
                state.draft.base_url += "/v1";
                state.base_url_set = true;
                state.models_valid = false;
                return TryFetchAndPick(io, state);
            }
            --index;  // 后面的选项整体前移一格
        }
        switch (index) {
            case 0:
                return RunManualModelInput(io, state, summary);
            case 1:
                return Goto(ProviderWizardStep::Wire);  // 直达接口格式,不层层倒退
            case 2:
                return Goto(ProviderWizardStep::BaseUrl);
            default:
                state.models_valid = false;
                return TryFetchAndPick(io, state);  // 重试
        }
    }
}

// ---------------------------------------------------------------------------
// 推理档位 / 额外参数(可选步,回车保留旧值)
// ---------------------------------------------------------------------------

StepResult RunEffortStep(WizardIO& io, ProviderWizardState& state) {
    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.progress = trf("provider_wizard.progress", 6, kProviderWizardStepCount);
    frame.body = {tr("provider_wizard.effort.hint")};
    if (state.effort_set && !state.draft.model_reasoning_effort.empty()) {
        frame.body.push_back(trf("provider_wizard.current_value", state.draft.model_reasoning_effort));
    }
    frame.error = state.last_error;
    frame.prompt = tr("provider_wizard.effort.prompt");
    frame.footer = tr("provider_wizard.footer.back");

    const WizardInputEvent event = ReadTextEvent(io, frame);
    if (event.kind == WizardInputEvent::Kind::Back) {
        return Goto(ProviderWizardStep::Model);
    }
    if (event.kind != WizardInputEvent::Kind::Submitted) {
        return Cancel();
    }
    if (event.text.empty()) {
        if (!state.effort_set) {
            state.draft.model_reasoning_effort.clear();
        }
    } else {
        state.draft.model_reasoning_effort = event.text;
    }
    state.effort_set = true;
    state.last_error.clear();
    return AfterEdit(state, ProviderWizardStep::ExtraBody);
}

StepResult RunExtraBodyStep(WizardIO& io, ProviderWizardState& state) {
    WizardFrame frame;
    frame.title = WizardTitle(state);
    frame.progress = trf("provider_wizard.progress", 7, kProviderWizardStepCount);
    frame.body = {tr("provider_wizard.extra_body.hint")};
    if (!state.draft.extra_body.empty()) {
        frame.body.push_back(trf("provider_wizard.current_value",
                                 trf("provider_wizard.extra_body.summary",
                                     state.draft.extra_body.size())));
    }
    frame.error = state.last_error;
    frame.prompt = tr("provider_wizard.extra_body.prompt");
    frame.footer = tr("provider_wizard.footer.back");

    const WizardInputEvent event = ReadTextEvent(io, frame);
    if (event.kind == WizardInputEvent::Kind::Back) {
        return Goto(ProviderWizardStep::Effort);
    }
    if (event.kind != WizardInputEvent::Kind::Submitted) {
        return Cancel();
    }
    if (event.text.empty()) {
        if (!state.extra_body_set) {
            state.draft.extra_body = nlohmann::json::object();
        }
        state.extra_body_set = true;
        state.last_error.clear();
        return AfterEdit(state, ProviderWizardStep::Confirm);
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(event.text);
    } catch (const nlohmann::json::parse_error& e) {
        state.last_error = trf("provider_wizard.extra_body.invalid_json", e.what());
        return Stay();
    }
    if (!parsed.is_object()) {
        state.last_error = tr("provider_wizard.extra_body.not_object");
        return Stay();
    }
    state.draft.extra_body = std::move(parsed);
    state.extra_body_set = true;
    state.last_error.clear();
    return AfterEdit(state, ProviderWizardStep::Confirm);
}

// ---------------------------------------------------------------------------
// 确认页(第 8/8 步):汇总带项号,输项号跳回单项修改;回车确认写入;
// n/N 放弃;Esc 回额外参数步。
// ---------------------------------------------------------------------------

std::vector<std::string> SummaryLines(const ProviderWizardState& state) {
    if (state.edit_mode) {
        return ProviderEditDiffLines(state.original, state.draft);
    }
    const config::ProviderConfig& p = state.draft;
    return {
        trf("provider_wizard.summary.name", p.name),
        trf("provider_wizard.summary.wire", config::ProviderWireName(p.wire)),
        trf("provider_wizard.summary.base_url", p.base_url),
        trf("provider_wizard.summary.auth", AuthDisplayLine(p)),
        trf("provider_wizard.summary.model", p.model),
        trf("provider_wizard.summary.effort",
            p.model_reasoning_effort.empty() ? tr("provider_wizard.effort.unset")
                                             : p.model_reasoning_effort),
        trf("provider_wizard.summary.extra_body",
            p.extra_body.empty() ? tr("provider_wizard.extra_body.unset")
                                 : trf("provider_wizard.extra_body.summary", p.extra_body.size())),
    };
}

StepResult RunConfirmStep(WizardIO& io, ProviderWizardState& state) {
    while (true) {
        WizardFrame frame;
        frame.title = WizardTitle(state);
        frame.progress = trf("provider_wizard.progress", 8, kProviderWizardStepCount);
        frame.body = SummaryLines(state);
        frame.body.push_back(tr("provider_wizard.confirm.hint"));
        frame.error = state.last_error;
        frame.prompt = tr("provider_wizard.confirm.prompt");
        frame.footer = tr("provider_wizard.footer.back");

        const WizardInputEvent event = ReadTextEvent(io, frame);
        if (event.kind == WizardInputEvent::Kind::Back) {
            return Goto(ProviderWizardStep::ExtraBody);
        }
        if (event.kind != WizardInputEvent::Kind::Submitted) {
            return Cancel();
        }
        if (event.text.empty() || event.text == "y" || event.text == "Y") {
            return Save();
        }
        if (event.text == "n" || event.text == "N") {
            return NoSave();
        }
        // 数字 = 跳回对应单项改,改完回汇总,后续无关步骤不重走。
        bool is_number = !event.text.empty() &&
                         event.text.find_first_not_of("0123456789") == std::string::npos;
        if (!is_number) {
            state.last_error = tr("provider_wizard.confirm.bad_number");
            continue;
        }
        int n = 0;
        try {
            n = std::stoi(event.text);
        } catch (...) {
            n = 0;
        }
        if (n < 1 || n > 7) {
            state.last_error = tr("provider_wizard.confirm.bad_number");
            continue;
        }
        state.last_error.clear();
        if (state.edit_mode && n == 1) {
            // 名字锁死:第 1 项不给跳,明说不支持改名(规格:改名=删旧建新,
            // 风险另议)。
            state.last_error = tr("provider_wizard.edit.no_rename");
            continue;
        }
        state.return_to = ProviderWizardStep::Confirm;  // 改完直接回汇总
        return Goto(ProviderWizardStepAt(static_cast<std::size_t>(n - 1)));
    }
}

// ---------------------------------------------------------------------------
// 主循环:step enum 驱动,前进/后退/跳转/取消/确认全在这一个 switch 里。
// add 与 edit 两套起手(ProviderWizardState 摆好)共用,别另立第二套循环。
// ---------------------------------------------------------------------------
std::optional<ProviderWizardOutcome> RunWizardLoop(WizardIO& io, ProviderWizardState& state) {
    while (true) {
        // 这一步开跑前有没有回程票:汇总页跳回单项时,票是跳转那一步刚发的,
        // 不能撕;早先剩下的票(用户在跳回后又自己往回走)才作废。
        const bool had_return_ticket = state.return_to.has_value();
        StepResult result;
        switch (state.step) {
            case ProviderWizardStep::Name:
                result = RunNameStep(io, state);
                break;
            case ProviderWizardStep::Wire:
                result = RunWireStep(io, state);
                break;
            case ProviderWizardStep::BaseUrl:
                result = RunBaseUrlStep(io, state);
                break;
            case ProviderWizardStep::Auth:
                result = RunAuthStep(io, state);
                break;
            case ProviderWizardStep::Model:
                result = RunModelStep(io, state);
                break;
            case ProviderWizardStep::Effort:
                result = RunEffortStep(io, state);
                break;
            case ProviderWizardStep::ExtraBody:
                result = RunExtraBodyStep(io, state);
                break;
            case ProviderWizardStep::Confirm:
                result = RunConfirmStep(io, state);
                break;
            default:
                return std::nullopt;
        }
        switch (result.action) {
            case StepResult::Action::Stay:
                continue;
            case StepResult::Action::Goto:
                if (had_return_ticket &&
                    ProviderWizardStepIndex(result.target) < ProviderWizardStepIndex(state.step)) {
                    // 跳回单项后又自己往回走 = 用户在导航,旧回程票作废。
                    // (刚从汇总页发的新票不在此列:had_return_ticket 那会儿
                    // 它还没发出来。)
                    state.return_to.reset();
                }
                if (result.target != state.step) {
                    state.last_error.clear();  // 换步不带旧错
                }
                state.step = result.target;
                continue;
            case StepResult::Action::Cancel:
                return std::nullopt;
            case StepResult::Action::Save:
                return ProviderWizardOutcome{state.draft, true};
            case StepResult::Action::NoSave:
                return ProviderWizardOutcome{state.draft, false};
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 导出的纯小工具
// ---------------------------------------------------------------------------

std::size_t ProviderWizardStepIndex(ProviderWizardStep step) {
    return static_cast<std::size_t>(step);
}

ProviderWizardStep ProviderWizardStepAt(std::size_t index) {
    if (index >= kProviderWizardStepCount) {
        index = kProviderWizardStepCount - 1;
    }
    return static_cast<ProviderWizardStep>(index);
}

std::string SuggestProviderSlug(const std::string& raw) {
    std::string slug;
    bool pending_dash = false;
    for (const char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '_' || c == '.' || c == '-') {
            if (pending_dash && !slug.empty() && slug.back() != '-') {
                slug += '-';
            }
            pending_dash = false;
            slug += c;
        } else if (uc > 0x7F) {
            // 中文这类多字节字符:整个折成一枚短横线(只建议,不暗改)。
            pending_dash = true;
        } else {
            pending_dash = true;  // 空格/斜杠等:折成短横线
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug;
}

std::string DefaultKeyEnvForWire(config::Wire wire) {
    return wire == config::Wire::Anthropic ? "ANTHROPIC_AUTH_TOKEN" : "OPENAI_API_KEY";
}

bool IsLocalBaseUrl(const std::string& base_url) {
    const std::size_t scheme = base_url.find("://");
    std::string host = scheme == std::string::npos ? base_url : base_url.substr(scheme + 3);
    // 去掉路径与端口,只留主机名。
    for (const char cut : {'/', ':', '?'}) {
        const std::size_t pos = host.find(cut);
        if (pos != std::string::npos) {
            host = host.substr(0, pos);
        }
    }
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]") {
        return true;
    }
    if (host.rfind("192.168.", 0) == 0 || host.rfind("10.", 0) == 0 ||
        (host.rfind("172.", 0) == 0 && host.size() > 5)) {
        // 172.16-31.x.x 才是私网;粗认 172.* 够用(只挪光标,不算定论)。
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 两个入口:add 起手从名字步往前走;edit 起手直接进汇总页,全字段预填。
// 主循环是同一个 RunWizardLoop,行为差异全由 ProviderWizardState 摆的
// edit_mode + 预填值 + 起手步决定。
// ---------------------------------------------------------------------------

std::optional<ProviderWizardOutcome> RunProviderAddWizard(
    WizardIO& io, const std::string& name_prefill, const std::vector<config::ProviderConfig>& existing) {
    ProviderWizardState state;
    state.existing = existing;
    state.draft.key_env = DefaultKeyEnvForWire(state.draft.wire);

    // 命令行给的名字:合法直接跳过第一问;不合法打一行提示再问。
    if (!name_prefill.empty()) {
        const auto valid = config::ValidateProviderName(name_prefill, existing);
        if (valid.has_value()) {
            state.draft.name = name_prefill;
            state.name_set = true;
            state.step = ProviderWizardStep::Wire;
        } else {
            io.print(trf("provider_wizard.name.prefill_invalid", name_prefill, valid.error()));
        }
    }

    return RunWizardLoop(io, state);
}

std::optional<ProviderWizardOutcome> RunProviderEditWizard(WizardIO& io,
                                                            const config::ProviderConfig& provider) {
    ProviderWizardState state;
    state.edit_mode = true;
    state.draft = provider;    // 全字段预填:context_window/extra_headers 这类
                               // 向导不碰的字段也一并坐船,写盘原样带回去
    state.original = provider;  // 确认页 diff 的对照底
    state.name_set = true;
    state.wire_set = true;
    state.base_url_set = true;
    state.auth_set = true;
    state.model_set = true;
    state.effort_set = true;
    state.extra_body_set = true;
    state.key_env_explicit = true;  // 配置里带的 key_env 是显式值,回车保留
    state.models_valid = false;  // 编辑模式模型步回车=保留,列表缓存压根不用
    state.step = ProviderWizardStep::Confirm;  // 起手直接进汇总页
    return RunWizardLoop(io, state);
}

std::vector<std::string> ProviderEditDiffLines(const config::ProviderConfig& original,
                                               const config::ProviderConfig& draft) {
    const config::ProviderConfig& o = original;
    const config::ProviderConfig& p = draft;
    // 改了的字段打"旧 → 新",没改的原样。名字行永远打"(不支持改名)"尾注。
    const auto effort_of = [](const config::ProviderConfig& c) {
        return c.model_reasoning_effort.empty() ? tr("provider_wizard.effort.unset")
                                                : c.model_reasoning_effort;
    };
    const auto extra_of = [](const config::ProviderConfig& c) {
        return c.extra_body.empty() ? tr("provider_wizard.extra_body.unset")
                                    : trf("provider_wizard.extra_body.summary", c.extra_body.size());
    };
    std::vector<std::string> lines;
    bool any_changed = false;
    const auto field = [&any_changed](bool changed, const char* same_key, const char* diff_key,
                                      const std::string& old_value, const std::string& new_value) {
        any_changed = any_changed || changed;
        return changed ? trf(diff_key, old_value, new_value) : trf(same_key, new_value);
    };
    lines.push_back(trf("provider_wizard.edit.name_locked", p.name));
    lines.push_back(field(o.wire != p.wire, "provider_wizard.summary.wire", "provider_wizard.edit.diff.wire",
                          config::ProviderWireName(o.wire), config::ProviderWireName(p.wire)));
    lines.push_back(field(o.base_url != p.base_url, "provider_wizard.summary.base_url",
                          "provider_wizard.edit.diff.base_url", o.base_url, p.base_url));
    // 鉴权行连着 key_env/api_key 一起比,展示走 AuthDisplayLine(掩码)。
    lines.push_back(field(AuthDisplayLine(o) != AuthDisplayLine(p) || o.auth != p.auth,
                          "provider_wizard.summary.auth", "provider_wizard.edit.diff.auth",
                          AuthDisplayLine(o), AuthDisplayLine(p)));
    lines.push_back(field(o.model != p.model, "provider_wizard.summary.model",
                          "provider_wizard.edit.diff.model", o.model, p.model));
    lines.push_back(field(o.model_reasoning_effort != p.model_reasoning_effort,
                          "provider_wizard.summary.effort", "provider_wizard.edit.diff.effort",
                          effort_of(o), effort_of(p)));
    lines.push_back(field(o.extra_body != p.extra_body, "provider_wizard.summary.extra_body",
                          "provider_wizard.edit.diff.extra_body", extra_of(o), extra_of(p)));
    if (!any_changed) {
        lines.push_back(tr("provider_wizard.edit.diff_none"));
    }
    return lines;
}

// ---------------------------------------------------------------------------
// 预设向导:目录选厂家 → 名字 → 密钥来源(三态)→ 确认。复用同一套
// 导航原语(事件/帧/选择);不带返回,先把 provider add 做稳。
// ---------------------------------------------------------------------------

std::optional<ProviderWizardOutcome> RunProviderPresetWizard(
    WizardIO& io, const config::ProviderCatalog& catalog, const std::string& name_prefill,
    const std::vector<config::ProviderConfig>& existing) {
    if (catalog.providers.empty()) {
        return RunProviderAddWizard(io, name_prefill, existing);
    }

    {
        std::vector<WizardChoiceItem> items;
        items.reserve(catalog.providers.size() + 1);
        for (const auto& preset : catalog.providers) {
            items.push_back({preset.name, preset.description});
        }
        items.push_back({tr("provider_catalog.choose.custom"), {}});
        WizardFrame frame;
        frame.title = tr("provider_catalog.choose.title");
        frame.footer = tr("provider_wizard.footer.first");
        const ChoiceNav choice = ReadChoiceNav(io, frame, items, 0);
        if (!choice.index.has_value()) {
            return std::nullopt;
        }
        if (*choice.index == catalog.providers.size()) {
            return RunProviderAddWizard(io, name_prefill, existing);
        }
        const config::ProviderPreset& preset = catalog.providers[*choice.index];
        config::ProviderConfig provider = config::ProviderConfigFromPreset(preset);

        // 名字:默认用预设 id,回车确认。
        std::string proposed = name_prefill.empty() ? preset.id : name_prefill;
        while (true) {
            const auto valid = config::ValidateProviderName(proposed, existing);
            if (valid.has_value()) {
                break;
            }
            WizardFrame name_frame;
            name_frame.title = tr("provider_catalog.choose.title");
            name_frame.body = {tr("provider_wizard.name.hint"),
                               valid.error()};
            name_frame.prompt = tr("provider_wizard.name.prompt");
            name_frame.footer = tr("provider_wizard.footer.first");
            const WizardInputEvent event = ReadTextEvent(io, name_frame);
            if (event.kind != WizardInputEvent::Kind::Submitted) {
                return std::nullopt;
            }
            if (event.text.empty()) {
                continue;
            }
            proposed = event.text;
        }
        provider.name = proposed;

        io.print(trf("provider_catalog.selected", preset.name, config::ProviderWireName(preset.wire),
                     preset.default_model));

        // 密钥来源:三态选择,默认变量名来自预设。预设流程没有"上一步"可回,
        // Esc(Back)按取消整个预设向导处理。
        ProviderWizardState auth_state;
        auth_state.draft = provider;
        auth_state.draft.key_env = preset.key_env.empty() ? DefaultKeyEnvForWire(provider.wire)
                                                          : preset.key_env;
        // 预设声明的变量名是显式值:env 子页回车保留,不按 wire 复位。
        auth_state.key_env_explicit = !preset.key_env.empty();
        StepResult auth_result = RunAuthStep(io, auth_state);
        if (auth_result.action != StepResult::Action::Goto || !auth_state.auth_set) {
            return std::nullopt;  // 取消/Esc/EOF,半个预设不落盘
        }
        provider = auth_state.draft;

        // 确认。
        ProviderWizardState confirm_state;
        confirm_state.draft = provider;
        while (true) {
            WizardFrame confirm_frame;
            confirm_frame.title = tr("provider_wizard.title");
            confirm_frame.progress = trf("provider_wizard.progress", 8, kProviderWizardStepCount);
            confirm_frame.body = SummaryLines(confirm_state);
            confirm_frame.body.push_back(trf("provider_wizard.summary.window",
                                             provider.context_window_tokens));
            confirm_frame.body.push_back(tr("provider_wizard.confirm.hint"));
            confirm_frame.error = confirm_state.last_error;
            confirm_frame.prompt = tr("provider_wizard.confirm.prompt");
            confirm_frame.footer = tr("provider_wizard.footer.back");
            const WizardInputEvent event = ReadTextEvent(io, confirm_frame);
            if (event.kind != WizardInputEvent::Kind::Submitted) {
                return std::nullopt;
            }
            if (event.text.empty() || event.text == "y" || event.text == "Y") {
                return ProviderWizardOutcome{provider, true};
            }
            if (event.text == "n" || event.text == "N") {
                return ProviderWizardOutcome{provider, false};
            }
            confirm_state.last_error = tr("provider_wizard.confirm.bad_number");
        }
    }
}

}  // namespace lubancode::cli
