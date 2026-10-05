# 结果仓不可变发布：File 默认实现前置

基 `80c8cd5a`。仅改 platform/atomic_write.hpp/.cpp、trajectory/v3/result_store.hpp/.cpp、本合同及新原生册。不开公共 Store，不改 Job／bridge／reader／CI，不造数据库或第二 Session 栈。原 AtomicWriteFile 替换函数与旧测试逐字保留；新口只复用其私有文件／目录刷盘原语和 WriteOutcome，绝不调用替换入口冒充不可变发布。

新 `CreateImmutableFileDetailed` 以实际 exclusive create 打开 PID＋序号唯一临时件；名字不等于排他，碰到旧临时件不能截断。临时件与目标同目录，完整 write、flush／文件确认、close 成功后，POSIX用link、Windows用不带REPLACE的MoveFileEx发布。正式名已存在（含同内容、链接或目录）即拒，不先exists再rename、不删正式件换成功。只清自己的临时件；并发／重开都靠原生no-replace，不靠扫描时间窗。

详细回执持实际请求档、WriteOutcome、稳定错误与逐阶段原件：temp open、写入字节数、stdio flush、文件sync、file close、publish、parent open/sync/close和临时件清理。真实返回与既有受控刷盘失败分别记录；测试旗替代同步动作时不伪造“OS调用失败”。新口没有存储callback或后台线程。close失败不得发布，发布后任何目录确认失败保CommittedDurabilityUnconfirmed，正式件和首错误原样留，不readback升级。

耐久范围必须说清：本口只确认文件数据与直接父目录；回执列出这份父目录，并明示未确认整个祖先链。ResultStore原Open建目录语义保留，首建session/artifacts祖先不能凭leaf fsync声称整棵namespace断电持久。目录建立的完整owner合同属后续Session Store租约，本笔不暗补全局目录事务。裸文件名先定位实际当前父目录，新口不把“没有父路径”当目录确认成功。

ResultStore的Persist/PersistListing真实接此口，保原结果ID、capture-/res-命名、编号扫描和JSON／预览算法。Persist尾添owned publication报告；Listing增Detailed入口，旧expected<string,string>薄转接。旧ok只在整笔所有文件都确认后为true；error含首稳定码和真实阶段，不把未知称NotCommitted。多文件已发布一部分而后失败，整笔按Indeterminate留残件，并逐枚保实际outcome，不宣称回滚。

同一Store在首次失败后封新发布，后续写请求只返回首失败报告，不重做、不读回升级；报告在首个native调用前已有owner，异常无返回receipt时照实留未知。成功仍按原编号递增。新Store重开可沿原扫描续号，但既有logical名／孤件依旧不可覆盖。析构没有待刷写句柄，不补确认；原子写每次先收实际file/dir句柄再返回。

旧调用者仍见ok/error，至少不会把未确认文件宣称结果持久化成功；各业务层的Unknown分类完整化后续接，不在这里改其他作者文件。所有原CASE、输入／超时／路径长度断言不改。新增一册test_v3_result_immutable_publication.cpp，真实文件及并发no-replace，核各阶段回执、受控文件/目录刷盘失败、首次未知留件与零重写、多文件部分发布、listing与重开拒覆写。文件操作只在远端native跑；本地仅文本、源码守卫、纯数据。
