# SDK Managed 项目登记寿命

本片补公开存储视图合同：同一 Runtime 内，完整 tenant/project/workspace/
bindingVersion 对应一份固定项目配置。Project 句柄退出不撤登记；场关闭、
View 退尽也不许拿原 binding/version 换 Policy 或 cwd。

Runtime 强持项目配置，直到 Shutdown。重复交原配置可取得新句柄；交同一
binding/version 却换 Policy 或 cwd，返回 `sdk.managed.project_conflict`。
以后若要换配置，须另走显式版本与迁移合同，不能借弱引用过期暗换权限源。

沿原监督登记与关闭次序收场。Shutdown 在锁内移出项目和场登记，在锁外
释放；Policy 最后退出仍守原回调门，不增一套拥有图或注销接口。
Managed 关场错误在交公共回执、存入 Runtime 首错前转为稳定码；内部原生
回执仍保原事实。Local 原诊断照留，混合 Runtime 不替 Local 改写错误正文。

原十三场授权验收保住。补实际项目句柄退尽、原配置重新登记、换 Policy
仍拒绝；再核实际场关闭、所有 View 退尽后，旧版本仍不能换权限源。
本地只查源码与纯数据；编译和原生验收交同源远端 CI。
