# 子历史采用册阶段诊断

本笔只添夹具诊断，尚未实施或验收。基线固定 Package 前置门 `7aa0f4b135b289d1dd40f329aae69cbe52356950`，不改生产、预算、权限或历史采用规则。

## 原件与证据边界

CI `37094285934`、Windows job `111121026722` 实际 checkout 为 `3717ebde83007c53e95e056acf0792a210e18656`，与源树 `fa05c07d08ebae4279c58028deaee49b7012587e` 相同。

完整原日志 SHA-256 为 `0728df20dc17fc32e258b008374051476880dc82ac23c20455700eaafc094f74`。第 18201 行记 `unit.runtime.child_history_adoption` Timeout 180.06 秒；18494–18507 行保留实际命令、起止时间和空 Output。真实命令仍选 `test_child_history_adoption.cpp`。原件未打印案号、阶段或线程，不能断言卡在模型、读取、关场或析构，也不称它为偶发噪声。

这份来源此前 SDK-focused 通过，只能证明此前那次执行。它不能替这次全量 unit 超时。另一处 ResultStore 失败另有合同，本笔不修它。

## 只添哪些诊断

原八案各有固定短 ID，诊断只含案号、该案 Rig 序号和固定阶段。标明案起止、RealRun、CheckHistory、CloseParent 前后，以及实际 Rig owner 析构前后。每行写 stderr，及时刷出；不用案尾 cout 是否已刷出推断进度。

输出设固定长度和行数上限，不打印模型请求、工具正文、账行、路径、凭据或环境。析构诊断不得抛异常，不得盖掉原故障，也不得提前 reset、join、Close 或改变既有成员退场次序。只在本历史册显式开启；共用 fixture 其它来源默认不添输出。

落点限 `tests/unit/runtime/test_child_history_adoption.cpp` 与共用 fixture 中所需的可选诊断标记。后者只夹住原 owner 退场次序，不新增 worker、锁、等待闸或借用回调。固定案号使用静态值；既有 owner、借用与文件寿命保持原路。

八个 TEST_CASE、全部断言、原子/错误码、真实模型和工具次数、180 秒 unit 帽、SDK-focused 帽及原八枚成功 marker 全部保留。不加 sleep，不增时限，不重排场景；本笔既不声称修好了超时，也不把超时改成成功。

## 远端验收

三平台新头执行原完整来源。分别保存真实 full/unit 与 SDK-focused 的完整 argv、注册、JUnit、LastTest 和 job 原日志；失败路径也保留原件。全量来源仍实选原八案，成功时须八案、非零断言、无跳过；超时则保实际已刷阶段，不能给未到达阶段补记录。

若需补 CI 留件，只保存该次实际全量与原 SDK 执行资料，不新增另跑一册来抵全量失败，不减原 roster。ASan 仍沿原必需来源执行；不从诊断推出 TSan、LSan 或跨进程验证。

本地只做静读、纯数据与文档检查，绝不 configure、编译、CTest 或运行原生夹具。共享功能分支和 main 不动；诊断头仍开 Draft，待新原件指出停点，再另定修正。

