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

}  // namespace lubancode::cli
