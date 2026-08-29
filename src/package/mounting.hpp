// 会话钉快照与内容组件挂载(统一 Package 封装单阶段 3)。
//
// 首版语义(单子 §十二"会话钉快照",契约 packages.md §9):启动时扫四层、
// 逐包 AnalyzePackage 一次,把 valid 包的内容组件折成四张表的挂载材料;
// 运行中目录变化不热生效——下回启动才见。不做热重载,不监听目录。
//
// 只挂内容组件(单子 §9.1):Skill、Workflow、Agent、Prompt Profile。带
// Plugin/MCP 的包照样挂它的内容组件,代码组件一件不挂不起(信任门阶段 4
// 在此收口;挂载事务是阶段 5),账上记 code_trust。包未过信任门时,引
// 用包内 Plugin/MCP 的 Agent、Workflow 标 unavailable 并注明缘由——登记
// 不是放行,依赖也不是可用。
//
// 挂载路数(单子 §三"正路"):Package 先产分析账(AnalyzePackage),这里
// 把组件根与成品定义交给原有加载器/Catalog——SkillCatalog、AgentCatalog、
// WorkflowCatalog、PromptProfileResolver。加载器不反过来扫 Package;这里
// 也不复制任何组件 schema。
//
// canonical 折名:packaged 组件一律 <包id>:<名> 登册;包内短引用
// (agent 的 prompt.profile / skills.preload,workflow 的 agent/skill/
// subflow)在本包有同名组件时折成 canonical 再交出去,外部裸引用与显式
// 全名原样保留(契约 §6"Resolver 先在本包里找")。
//
// 阶段 6 在这层升两件东西:PackageSnapshot(把"启动装配一次"的隐式语义
// 升成显式快照对象——会话与 reload 各折一份,在跑引用钉住各自那份,目录
// 突变不影响)与启停门(停用的包挂载一律跳过,连内容组件都不挂)。
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"      // PackagedAgentEntry
#include "agent/prompt_assembler.hpp"   // PackageProfileRoot
#include "package/catalog.hpp"          // AnalyzePackage 的产物
#include "package/state.hpp"            // PackageStateSnapshot(阶段 6 启停门)
#include "package/trust.hpp"            // PackageTrustSnapshot(阶段 4 信任门)
#include "tools/skill_loader.hpp"       // PackagedSkillRoot
#include "workflow/catalog.hpp"         // PackagedWorkflowSource

namespace lubancode::package {

// ---------------------------------------------------------------------------
// PackageMount:一场会话钉住的挂载快照。
// ---------------------------------------------------------------------------
struct PackageMountEntry {
    std::string package_id;
    std::string version_text;
    PackageScope scope = PackageScope::User;
    std::filesystem::path package_root;
    std::string content_hash;
    // 内容组件的 canonical id 清账(按 ComponentKind 目录序、local id 字节序)。
    std::vector<std::string> mounted_canonical_ids;
    // code 组件(Plugin/MCP)的门禁(阶段 4):Trusted = 过了门(挂载事务
    // 在阶段 5);PendingTrust = 一件不挂不执行,依赖它的 Agent/Workflow
    // 连坐 unavailable;NoCode = 包里没有。
    CodeTrustStatus code_trust = CodeTrustStatus::NoCode;
};

struct PackageMount {
    std::vector<PackageMountEntry> entries;  // 按 package_id 字节序;只收 valid 胜者
    // 各包的完整分析账(挂载材料从这折;也供 /package 命令对账)。会话钉
    // 住的就是这一份——运行中目录怎么变,这里的账不变。
    std::vector<PackageRecord> records;
    // 一件不挂的账(reload 回执与 /package 对账用,按 package_id 字节序):
    // rejected = 胜者但整包 invalid(清单坏/组件坏/引用悬空,诊断走
    // /package doctor);disabled_skipped = 停用被跳过的(扫描发现照旧)。
    std::vector<std::string> rejected_ids;
    std::vector<std::string> disabled_skipped_ids;

    bool empty() const { return entries.empty(); }
    const PackageMountEntry* Find(const std::string& package_id) const;
};

// 挂载输入:扫描四层的根 + 包外短引用的兜底账(standalone 技能名、
// config.json 的 mcpServers 键、builtin Agent 名——有就喂,与 /package
// doctor 的 BuildExternalNamespaces 同一口径;不喂则指向它们的短引用按
// 悬空判,整包 invalid)+ 信任账的只读快照(阶段 4:启动时从
// ~/.lubancode/package-trust.json 折一份,会话钉住;不喂 = 谁都没批)
// + 启停账的只读快照(阶段 6:停用的包挂载一律跳过;不喂 = 全启用)。
//
// store_candidates(自进化闭环阶段 4):evolution version store 的选中版本
// (active/canary 指针指到的那一枚)折成的现成候选,scope=Store。哈希验
// 完好没有由 evolution 侧负责——对不上(手改过 store 内文件)的根本不
// 递进来,这里照单全收。同 id 优先级:dev > project > store > user >
// official(store 压手装的用户层拷贝,让显式调试层)。
struct PackageMountInput {
    ScanOptions scan;
    ExternalNamespaces external;
    PackageTrustSnapshot trust;
    std::vector<PackageCandidate> store_candidates;
    PackageStateSnapshot state;
};

// 启动装配一次。次序:四层扫描 -> 同 id 按优先级定胜者(dev > project >
// store > user > official,被遮的不挂) -> 停用的跳过(连内容组件一件
// 不挂,阶段 6 启停门) -> 逐胜者 AnalyzePackage -> valid 的收进挂载账
//(invalid 一件不挂——整包成整包败)。
PackageMount BuildPackageMount(const PackageMountInput& input);

// ---------------------------------------------------------------------------
// PackageSnapshot:一场会话钉住的显式快照(单子 §12.3,阶段 6 把"启动
// 装配一次"的隐式语义升成真身)。
//
// 折成后不可变:路径、内容哈希、整份分析账(records 里组件定义全是解析
// 好的内存件)都在里头。在跑的 Agent/Workflow/Plugin/MCP 引用它折材料,
// 目录怎么变(删包、改 SKILL、disable)都影响不到——快照不回盘;新装配
// (新会话,或 /package reload 重折后的派发)才见新账。
//
// 折算只产数据不碰任何 runtime:reload 换快照不会起进程、不会卸模块
//——code 组件(Plugin/MCP)的挂载事务只在会话启动跑(阶段 5 语义),
// reload 回执须明说"code 组件须新会话"。
// ---------------------------------------------------------------------------
struct PackageSnapshot {
    PackageTrustSnapshot pinned_trust;   // 折这份快照时钉住的信任账(reload 复用:
                                         // code 门禁会话启动定终身,§7.1)
    PackageStateSnapshot state;          // 折快照时读到的启停账(回执对账用)
    std::string built_at_unix;           // 快照折成的时刻(unix 秒文本)
    int generation = 1;                  // 第几折:启动 = 1,reload 递增

    bool empty() const { return mount_.empty(); }
    const PackageMountEntry* Find(const std::string& package_id) const {
        return mount_.Find(package_id);
    }
    // 挂载账本体(entries + records + 一件不挂的账)。只读取用。
    const PackageMount& mount() const { return mount_; }

    // 包内技能正文(canonical id -> SKILL.md 正文):组件 parser 折快照时
    // 已读进 records,这里摊平成查表。在跑引用读快照拿正文,不回盘——
    // 盘中 SKILL.md 被删被改,已钉这份快照的会话照旧。standalone 技能
    // 不在快照里,照旧从盘上读。
    std::optional<std::string> SkillBody(const std::string& canonical_id) const;
    // 包内技能的 canonical id 全账(测试与对账用)。
    std::vector<std::string> SkillCanonicalIds() const;

private:
    friend std::shared_ptr<const PackageSnapshot> BuildPackageSnapshot(const PackageMountInput& input,
                                                                       int generation);
    PackageMount mount_;                            // 挂载账本体
    std::map<std::string, std::string> skill_bodies;  // canonical id -> body
};

// 折一份快照(纯函数:同输入同快照,generation 递增不影响内容)。启动
// 与 reload 都走这一只——reload 的原子性一半在这(另一半在"折好才换",
// 见 ReloadPackageSnapshot)。
std::shared_ptr<const PackageSnapshot> BuildPackageSnapshot(const PackageMountInput& input, int generation);

// ---------------------------------------------------------------------------
// /package reload 的核心:重折一份,折不动就一分不动。
//
// 原子在"折好才换"(单子阶段 6):全部折算落在一只崭新的快照对象上,
// 中途任何错(异常兜底)旧快照一字不动,回执带诊断;折成了才把新快照
// 交出去,由会话侧原子换档。信任账钉 current 那份——reload 不放行运行
// 中新批的 code 组件(它们要新会话),也避免"账面已信任、工具没挂"的
// 假可用。
// ---------------------------------------------------------------------------
struct PackageReloadReport {
    bool ok = false;
    std::string error;                // 折不动的一句话(旧快照未动)
    std::vector<std::string> lines;   // ok 时的回执(增/减/改、invalid、code 须新会话)
    std::shared_ptr<const PackageSnapshot> snapshot;  // ok 时才有
};

PackageReloadReport ReloadPackageSnapshot(const std::shared_ptr<const PackageSnapshot>& current,
                                          const PackageMountInput& fresh_input);


// ---------------------------------------------------------------------------
// 四张表的挂载材料:各自的原生系统收这些,不收 PackageMount 本体。
// 顺序稳定:按包 id、包内目录序;同一份 mount 折两遍,结果一致。
// ---------------------------------------------------------------------------

// Skill loader 的包根(<包根>/skills;source_level 带 scope 标签)。
std::vector<tools::PackagedSkillRoot> MountSkillRoots(const PackageMount& mount);

// AgentCatalog 的包层成品件(定义里的包内短引用已折 canonical)。引用了
// 未信任 Plugin/MCP 的 Agent 标 available=false + unavailable_reason(阶段
// 4 连坐:登记不是放行,依赖也不是可用)。
std::vector<agent::PackagedAgentEntry> MountAgentEntries(const PackageMount& mount);

// WorkflowCatalog 的包层成品件(id 换 canonical;包内 agent/skill/subflow
// 短引用已折 canonical;task/prompt/template 文件引用原样,dir 指包内)。
// 依赖未信任 code 组件(直接引工具,或经 unavailable 的 Agent)同样
// available=false + unavailable_reason。
std::vector<workflow::PackagedWorkflowSource> MountWorkflowSources(const PackageMount& mount);

// Prompt Profile 的包层根(<包根>/prompts/profiles,canonical 名在此解析)。
std::vector<agent::PackageProfileRoot> MountProfileRoots(const PackageMount& mount);

}  // namespace lubancode::package
