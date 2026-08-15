// 底栏的一本帧账(0.29.x"导航贴底并整帧去重"):空闲 composer 与流式
// footer 共用的帧描述。规格"整帧重画"一节写死——两条路不得各拼一套行序,
// 布局函数只产行与高度,终端 painter 只管锚点、擦除、落笔。
//
// 帧的行序(自上而下):
//   queue_rows(待发队列,composer 上横线之上)
//   上横线 / composer_rows 行输入 / 下横线
//   status_rows 行状态栏
//   agent_dock_rows(导航坞,贴底)
//   transient_rows(slash 提示等短命 UI,垫最底)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cli/agent_panel.hpp"

namespace lubancode::cli {

struct BottomChromeFrame {
    std::vector<std::string> queue_rows;       // 待发队列(空队列零行)
    std::vector<std::string> agent_dock_rows;  // 导航坞(无子代理零行)
    std::vector<std::string> transient_rows;   // slash 提示等(常态零行)
    int composer_rows = 1;                     // 输入行物理行数(软换行算在内)
    int status_rows = 1;                       // 状态栏行数
    int rule_rows = 2;                         // 上下横线
    int selected_task_id = 0;                  // 导航当前选中(0=main,-1=汇总哨兵)
    std::uint64_t revision = 0;                // 帧身份:内容变必变

    // 整帧行数(队列+横线+输入+状态+坞+提示)。
    int TotalRows() const {
        return static_cast<int>(queue_rows.size()) + composer_rows + rule_rows + status_rows +
               static_cast<int>(agent_dock_rows.size()) + static_cast<int>(transient_rows.size());
    }
    // 坞首行相对帧顶的偏移:队列之后、框与状态栏之下。
    int AgentDockFirstRow() const {
        return static_cast<int>(queue_rows.size()) + composer_rows + rule_rows + status_rows;
    }
};

// 帧指纹:行内容 + 选择 + 高度拼成一串,给"变了才重画"的比较用;revision
// 由同内容哈希而来(内容同则 revision 同,不引入额外状态)。
std::string BottomChromeFingerprint(const BottomChromeFrame& frame);
std::uint64_t BottomChromeRevision(const BottomChromeFrame& frame);

}  // namespace lubancode::cli
