// 初次配置向导:交互模式启动时,base_url 或 api_key(或 model)解不出来,
// 就走这一套问答,问完直接可用,不用重启。
//
// 逻辑写成纯函数 RunSetupWizard,除了三个注入点(打印一行、读一行、拉模型
// 列表)不碰任何真实 IO——生产代码接 std::cout / cli::ReadLine / api::ListModels,
// 单测接假的打印收集器 / 脚本化输入序列 / 假的模型列表结果,不真发网络请求。

#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "api/models.hpp"
#include "api/types.hpp"
#include "config/config.hpp"

namespace lubancode::cli {

// 打印一行文字(不含结尾换行,调用方自己决定怎么加)。
using WizardPrintFn = std::function<void(const std::string& line)>;

// 读一行输入,EOF 返回 std::nullopt。注意这里没有 prompt 参数——向导自己
// 通过 print 把提示语打出来,生产代码接的是
// `[]{ return lubancode::cli::ReadLine(""); }`(prompt 已经打过了,不用
// ReadLine 再打一遍)。
using WizardReadLineFn = std::function<std::optional<std::string>()>;

// 拉取模型列表的注入点,生产代码接 api::ListModels,单测注入假数据/假报错。
using WizardFetchModelsFn = std::function<std::expected<std::vector<api::ModelInfo>, api::Error>(
    config::Wire wire, const std::string& base_url, const std::string& api_key)>;

struct WizardIO {
    WizardPrintFn print;
    WizardReadLineFn read_line;
    WizardFetchModelsFn fetch_models;
    // 展示"保存到 XXX?"这一句用的路径文字,纯数据、不是 IO,调用方(main.cpp)
    // 自己拼好传进来(通常是 config::HomeDir() 拼出来的 .lubancode.json 路径)。
    std::string home_config_display_path = "<主目录>/.lubancode.json";
};

struct WizardOutcome {
    config::Config config;
    bool save_requested = false;  // 用户是否同意把配置写入 home_config_display_path
};

// 中途读到 EOF(read_line 返回 nullopt)按用户放弃处理,返回 std::nullopt。
std::optional<WizardOutcome> RunSetupWizard(WizardIO& io);

// ---------------------------------------------------------------------------
// 下面几个是向导用的通用小工具,原本关在 setup_wizard.cpp 的匿名命名空间里,
// /provider add 向导(cli/provider_wizard.cpp)复用同一套问答机器,所以搬出来
// 导出。行为不变,注释见 setup_wizard.cpp 里原来的位置。
// ---------------------------------------------------------------------------

// 剥掉尾部所有斜杠。
std::string StripTrailingSlashes(const std::string& s);

// 读一行、剥空白;EOF 时返回 std::nullopt。
std::optional<std::string> ReadTrimmed(WizardIO& io, const std::string& prompt);

// 反复问,直到读到非空字符串,或者 EOF(此时返回 std::nullopt)。
std::optional<std::string> ReadRequired(WizardIO& io, const std::string& prompt, const std::string& empty_hint);

// 编号选择,空输入按默认(1-based default_choice)处理,超范围/非数字重问。
// EOF 返回 std::nullopt。
std::optional<std::size_t> ReadChoice(WizardIO& io, const std::string& prompt, std::size_t count,
                                       std::size_t default_choice);

// model 这一步:输入非空就直接用;输入空就拉列表、编号选;拉取失败/列表为空
// 都回落到"手动输入,必须非空"。返回值为空表示 EOF,调用方原样往上传。
std::optional<std::string> ResolveModel(WizardIO& io, config::Wire wire, const std::string& base_url,
                                         const std::string& api_key);

}  // namespace lubancode::cli
