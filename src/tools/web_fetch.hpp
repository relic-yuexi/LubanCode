// web_fetch 工具:抓一个 URL 回来给模型看。HTML 剥掉标签取正文,普通文本
// 原样给,二进制(含 NUL 字节)拒收。深读长文不在这里做——系统提示里引导
// 模型把"搜了再读再总结"这类活交给 agent 子代理,web_fetch 只管取回来。

#pragma once

#include <cstddef>
#include <string>

#include "tools/tool.hpp"

namespace lubancode::tools {

// ---- 下面的纯函数不碰网络,单独导出好单测 ----

// 手写的小剥离器:HTML -> 正文。做四件事:
//   1) <script>/<style> 整块连内容一起剔掉,<!-- 注释 --> 同样剔掉;
//   2) 其余标签替换掉——块级标签(p/div/br/h1..h6/li/tr/…)替成换行,
//      行内标签替成空,保留段落结构;
//   3) 常见实体还原(&amp; &lt; &gt; &quot; &nbsp; &apos; &#39;);
//   4) 连续空白折叠:行内多个空格/tab 折成一个,三个以上连续换行折成
//      两个(段落间最多空一行)。
// 不追求把天下 HTML 都解析对——够模型读懂正文就行。
std::string StripHtml(const std::string& html);

// 把 text 按 UTF-8 边界截断到不超过 max_bytes 字节:落点若在多字节字符
// 中间,往回退到上一个完整字符结束处,绝不吐出半个字符(半个字符是非法
// UTF-8,塞进 JSON 请求体时 nlohmann 的 dump() 会直接抛异常)。实现走
// platform::Utf8PrefixBoundary,web 工具不自养字节刀。
std::string TruncateUtf8(const std::string& text, std::size_t max_bytes);

// 抓回正文的三步纯函数管线(不碰网络,清洗合同的测试口):
//   1) Content-Type 认 text/html 就先剥标签,普通文本原样;
//   2) 过 platform::SanitizeExternalText——网络正文是外来文本,与 MCP
//      rich result、Search 走同一替换合同(保合法 UTF-8 片段,坏字节换
//      U+FFFD;不再自养 '?' 清洗器);
//   3) 按公共 UTF-8 边界截断到 max_bytes。
struct PreparedBody {
    std::string text;
    bool truncated = false;
};
PreparedBody PrepareFetchedBody(const std::string& content_type, const std::string& raw_body,
                                std::size_t max_bytes);

class WebFetchTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::ReadOnlyRemote; }
    // user_agent 是 HTTP 请求头里报的身份(main.cpp 传 "lubancode/版本号");
    // 默认值只给单测和忘了传的调用方兜底。
    explicit WebFetchTool(std::string user_agent = "lubancode");

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    std::string user_agent_;
};

}  // namespace lubancode::tools
