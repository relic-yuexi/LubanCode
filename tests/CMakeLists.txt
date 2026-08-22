add_executable(lubancode_tests
  main.cpp
  test_sse_framing.cpp
  test_anthropic_events.cpp
  test_assembler.cpp
  test_tools.cpp
  test_background_tasks.cpp
  test_command_safety.cpp
  test_loop.cpp
  # 宽窄转换异常单(未映射 Unicode 字符掐死会话):SSE 劈半 delta 闸门、
  # emoji 路径 u8 通道、宽窄转换不许抛、后端抛异常的回合收口。
  test_unicode_boundaries.cpp
  test_responses_request.cpp
  test_responses_events.cpp
  test_chat_request.cpp
  test_chat_events.cpp
  test_write_edit.cpp
  test_ask_user.cpp
  test_search.cpp
  test_context.cpp
  test_context_events.cpp
  # 请求前缀守恒(前缀缓存单第三期):DiffRequests/IsAppendOnlySuccessor
  # 纯函数 + 捕获 backend 钉默认工具往返与六桩断裂源。
  test_request_prefix.cpp
  test_config.cpp
  test_model_catalog.cpp
  # 运行策略(子代理与 MainAgent 同级,根因一):输出预算三级解析与
  # main/子代理同有效值的铁律。
  test_runtime_profile.cpp
  test_provider_catalog.cpp
  test_update_checker.cpp
  test_models.cpp
  test_setup_wizard.cpp
  test_provider_wizard.cpp
  test_provider_switch.cpp
  test_slash_commands.cpp
  test_worktree.cpp
  test_worktree_tool.cpp
  test_isolation.cpp
  test_agent_isolation.cpp
  test_theme.cpp
  test_prompts.cpp
  test_prompt_assembler.cpp
  test_line_editor.cpp
  test_queue_model.cpp
  # 0.28.x 排队消息取回键:CSI 参数表(纯函数)、取回判定、备用键开关。
  test_queue_keys.cpp
  test_peer_mailbox.cpp
  test_peer_registry.cpp
  test_peer_loop.cpp
  test_peer_transport.cpp
  test_peer_tools.cpp
  test_terminal_frame.cpp
  test_choice_menu.cpp
  test_compact.cpp
  test_context_tracker.cpp
  test_anthropic_request.cpp
  test_agent_tool.cpp
  # 子代理活跃度/诊断日志/墙钟兜底/挂起服务真链路(规格《子代理活跃度
  # 不可见与疑似挂死》):活度账阶段与流式尾巴、LUBANCODE_DEBUG_SUBAGENT
  # 逐事件日志与隐私、墙钟收杀与强制收账、accept 后装死 socket 的
  # stream_idle_timeout 终态。
  test_agent_activity.cpp
  # agent_message 窄工具(主模型给运行中子代理传增量):入参/四类失败、
  # 运行名册、假 main backend 集成(inbox 收到 + 来源标签)。
  test_agent_message_tool.cpp
  test_hooks.cpp
  test_hooks_framework.cpp
  test_hooks_dispatcher.cpp
  test_skills.cpp
  test_mcp_transport.cpp
  test_mcp_config.cpp
  test_mcp_client.cpp
  test_mcp_tool.cpp
  test_lsp_framing.cpp
  test_lsp_config.cpp
  test_lsp_client.cpp
  test_lsp_tool.cpp
  test_todo_tool.cpp
  test_tool_search.cpp
  # 工具文案按语言分档(模型可见文字抽离 C++):三级回退、缺省零变化、
  # en 生效、语言切换即时。
  test_tool_text.cpp
  test_todo_render.cpp
  test_divider.cpp
  test_format_utils.cpp
  test_transcript.cpp
  test_agent_panel.cpp
  # 0.29.x 底栏一本帧账:行序/高度/指纹纯逻辑(空闲与流式共用)。
  test_bottom_chrome.cpp
  # 交互抛光总账第一步:Action/Keymap/Capability 三层地基。
  test_keymap.cpp
  test_terminal_caps.cpp
  # 第二批:Ctrl+R 提问历史反向搜索(索引/过滤/状态机/渲染行)。
  test_history_search.cpp
  # 第三批:@ 提及菜单纯逻辑 + 编辑器整枚删 + 编辑器草稿归一。
  test_mention_menu.cpp
  test_agent_inbox.cpp
  test_markdown.cpp
  test_latex_math.cpp
  test_diff.cpp
  test_diff_syntax.cpp
  test_plugins.cpp
  test_web_fetch.cpp
  test_web_search.cpp
  test_session_store.cpp
  # 会话管理器单第一步:SessionCatalog 中立查询层(排序键/摘要/两排序/两
  # scope/搜索/分页/坏档/空表/指纹缓存)。
  test_session_catalog.cpp
  # 第二步:SessionPicker 纯逻辑(焦点轮换/搜索/筛选排序切换/翻页边界/
  # 选中保持/视口窗口账/渲染帧/空态/相对时间/百分比)。
  test_session_picker.cpp
  test_prompt_files.cpp
  test_project_instructions.cpp
  test_project_memory.cpp
  test_i18n.cpp
  test_image_input.cpp
  test_network_timeout.cpp
  test_skill_store.cpp
  test_workflow_recorder.cpp
  test_skill_drafter.cpp
  test_backend_stack.cpp
  test_tool_runtime.cpp
  test_status_refresh.cpp
  # app 编译边界钉子:两只 TU 同 include app 公共头,专查 ODR 与漏符号;
  # 相应实现由共享的 lubancode_app 静态库提供。
  test_app_boundary_a.cpp
  test_app_boundary_b.cpp
  # InteractiveSession 真构造/EOF 退场/真析构的集成钉子。
  test_interactive_session.cpp
  # ParseCliArgs 参数矩阵(纯函数)。
  test_cli_options.cpp
  # 领域命令 handler 的状态账(/clear /title /send /peerperm)。
  test_app_commands.cpp
  # /doctor 诊断纯函数(Effort 探针请求/四态分类/Prometheus 解析/固定前缀对账)。
  test_doctor.cpp
  # memory 回合抽取(分型/转写/JSON 解析,纯函数)。
  test_memory_extract.cpp
  # 模型分工第一期:三角色解析/回退链/任务映射/compact_model 兼容/
  # usage 分账//model roles 短表/跨 provider 路由(mock 全程)。
  test_model_router.cpp
  # 可追回 artifact(第二期):分块边界/原子落盘/hash 隔离/预算拒绝/
  # CompressWorkingView 落仓/两把只读钥匙的 scope。
  test_artifact_store.cpp
  # L2 microcompact(第三期):冷区挑选/触发线与迟滞/摘要解析/失败退 L1/
  # 应用趟与渲染(假 backend,mock 全程)。
  test_microcompact.cpp
  # ContextBudgetPlan(第四期):统一预算总账公式/窗口未知与超窗边界/
  # 摘要目标与压缩请求输入预算分账。
  test_context_budget.cpp
  # memory schema 3 front matter(往返字节稳定/坑人字符/双格式剥壳)。
  test_memory_frontmatter.cpp
  # memory 检索评测尺子(固定语料,Recall/Precision/注入字节/stale 拦截率)。
  test_memory_retrieval.cpp
  # memory 中文检索评测尺子(100 条中文语料,召回率/误命中率/注入字节 P50/P95)。
  test_memory_retrieval_zh.cpp
  # app 与终端实现由 lubancode_app 提供，不在测试目标里重复编译。
  # 回归 BuildCallbacks::on_usage 接线(状态行局部发布)仍走真实终端实现。
  test_repaint_coord.cpp
  test_utf8_boundary.cpp
  # PTC(Programmatic Tool Calling)P0:协议帧、stub 生成器、受限 runner
  # (假工具 + 真 Python)、五道上限、取消链、编码穿透。
  test_ptc_protocol.cpp
  test_ptc_stub.cpp
  test_ptc_runner.cpp
  # PTC P1:真工具完整链(schema/PreToolUse/权限/执行/PostToolUse/取消)、
  # 延迟挂载谓词、能力画像(指纹/auto 门槛/熔断器/存档)与 config 段。
  test_ptc_tool.cpp
  test_ptc_profile.cpp
  # PTC P2:LubanBench-Tool 八类场景离线层(题库/判卷/报告)。
  test_ptc_bench.cpp
  # app-server 无界面后台协议(阶段 1:协议骨架):schema 信封与错误码、
  # 分帧/有界出站队列、握手状态机与假连接整线、假 backend 整回合。
  test_app_server_schema.cpp
  test_app_server_connection.cpp
  test_app_server_dispatch.cpp
  test_app_server_turn.cpp
  # app-server 阶段 2:审批与打断(permission/request 四态、悬停/超时/
  # 打断、acceptForSession 免问、迟到失效、ask_user 的 user/ask)。
  # 显示系统剥离单第一步:runtime 合同(Event/Command/Broker 形状与序列化
  # 往返保真、稳定枚举字符串、坏报文抛错)。
  test_runtime_contract.cpp
  # 显示系统剥离单 P2:AgentLoop 异步审批通道(async 优先/同步回落/四态
  # 收账/悬空收口文案)。
  test_approval_broker.cpp
  # 显示系统剥离单 P3:TurnRuntime 纯逻辑核(权限裁定/取消/usage/prompt 门),
  # 脱离终端可驱动。
  test_turn_runtime.cpp
  # 显示系统剥离单 P4:ToolDisplay 认 tool_use_id 不认下标(交错终态/迟到
  # 事件/主子条目/确认块)。
  test_tool_display_id.cpp
  # plugins 本地 Tool 运行时第 1 步(冻结合同):manifest 强校验/标识符/
  # ${plugin_dir} 圈禁/协议帧/入参 Schema 子集/模型可见性,纯函数直测。
  test_plugin_contract.cpp
  # 第 2 步(process v1):自带 Python helper 的真机终态矩阵——正常往返/
  # 混日志/坏 JSON/call_id 不合/超时/ESC/非零退出/stderr 塞满/输出超限,
  # 缺 Python 整文件 SKIP。
  test_plugin_process.cpp
  # Workflows 自然语言编排单第 1 批:定义/解析 round-trip/内容 hash/
  # catalog 两级扫描与遮蔽/校验器全错类/图渲染,纯逻辑。
  test_workflow_definition.cpp
  # 第 2 批:Store/resolver、状态机迁移表、RunJournal 落盘与恢复、顺序图
  # 最小闭环(fake executor/fake clock)、重试/取消/预算/fallback 边。
  test_workflow_runtime.cpp
  # 第 3 批:parallel/join 五策略、稳定汇合顺序、并发帽、map/foreach/
  # reduce、switch 条件选路、并行取消传播(论文四路假工具)。
  test_workflow_parallel.cpp
  # 第 4 批:宿主执行器(真 ToolRegistry/假 backend/即时 broker)、
  # subflow 结果映射与错误隔离、journal 断点恢复。
  test_workflow_agents.cpp
  # 第 5 批:Draf 解析(杜撰工具/明文 secret 拦截)/追问计算/预览/
  # staging+rename 原子安装/回滚件/enable/remove/命令与参数解析。
  test_workflow_compiler.cpp
  # 第 6 批:受约束 GraphPatch(模板集/allowlist/append-only)、风险
  # 评定、WorkflowRunSnapshot 与增量事件(重连补账)。
  test_workflow_planner.cpp
)
target_link_libraries(lubancode_tests PRIVATE lubancode_app doctest::doctest)
target_compile_features(lubancode_tests PRIVATE cxx_std_23)
target_precompile_headers(lubancode_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/pch.hpp")
# main.cpp 须先定义 DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN，再独自包含 doctest；
# 若强塞测试 PCH，定义落在包含之后，入口便不会生成。
set_source_files_properties(main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if(MSVC)
  target_compile_options(lubancode_tests PRIVATE /MP /FS)
endif()

# M11:test_network_timeout.cpp 手写了个原始 socket 假服务器(模拟连不上/
# 半路断流),要链 Winsock。
if(WIN32)
  target_link_libraries(lubancode_tests PRIVATE ws2_32)
endif()

# M8:真实 Python 夹具(mcp_test_server.py)测试要知道 tests/fixtures/ 的绝对
# 路径——ctest 跑的时候工作目录不一定是 tests/ 源码目录,不能瞎猜相对路径。
target_compile_definitions(lubancode_tests PRIVATE
  LUBANCODE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  LUBANCODE_TEST_OFFICIAL_SKILLS_DIR="${CMAKE_SOURCE_DIR}/skills"
  LUBANCODE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# M7:真 DLL 加载测试的靶子——示例插件 hello_plugin(直接编 examples/ 里那
# 份源码,顺带验证示例真的能编)+ api_version 填错的 bad_version_plugin。
# 两个 DLL 落在同一个输出目录,目录路径喂给测试(LUBANCODE_TEST_PLUGIN_DIR),
# PluginHost::LoadDirectory 真扫真载。
if(WIN32)
  add_library(hello_plugin_fixture SHARED
    ${CMAKE_SOURCE_DIR}/examples/plugins/hello_plugin/hello_plugin.c)
  target_include_directories(hello_plugin_fixture PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(hello_plugin_fixture PROPERTIES OUTPUT_NAME hello_plugin PREFIX "")

  add_library(bad_version_plugin SHARED fixtures/bad_version_plugin.c)
  target_include_directories(bad_version_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(bad_version_plugin PROPERTIES PREFIX "")

  add_dependencies(lubancode_tests hello_plugin_fixture bad_version_plugin)
  target_compile_definitions(lubancode_tests PRIVATE
    LUBANCODE_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:hello_plugin_fixture>")
endif()

add_test(NAME lubancode_tests COMMAND lubancode_tests)

# 工具文案语言档端到端驱动(不进 ctest,验收手动跑):
#   tool_text_lang_driver <zh-CN|en>
# 把试点工具的模型可见文案按所选语言打到 stdout,供与迁移前原文逐字节
# 比对(缺省 zh-CN)与英文档生效验证(en)。
add_executable(tool_text_lang_driver EXCLUDE_FROM_ALL tool_text_lang_driver.cpp)
target_link_libraries(tool_text_lang_driver PRIVATE lubancode_core)
target_compile_features(tool_text_lang_driver PRIVATE cxx_std_23)

# DeepSeek 真机 e2e(前缀缓存守恒单):opt-in,不进 ctest——没设
# DEEPSEEK_API_KEY 自己就跳过(exit 0),要跑先导钥匙:
#   deepseek_e2e   (可配 DEEPSEEK_MODEL / DEEPSEEK_BASE_URL)
add_executable(deepseek_e2e EXCLUDE_FROM_ALL deepseek_e2e.cpp)
target_link_libraries(deepseek_e2e PRIVATE lubancode_core)
target_compile_features(deepseek_e2e PRIVATE cxx_std_23)

# 多行字符公式的视觉小样：直连正式渲染器，不进 ctest。
add_executable(latex_box_experiment EXCLUDE_FROM_ALL latex_box_experiment.cpp)
target_link_libraries(latex_box_experiment PRIVATE lubancode_core)
target_compile_features(latex_box_experiment PRIVATE cxx_std_23)

# 可追回 artifact(第二期)的布局实测驱动器:手动跑,验收记录用。
#   artifact_layout_dump <输出根目录>
add_executable(artifact_layout_dump EXCLUDE_FROM_ALL artifact_layout_dump.cpp)
target_link_libraries(artifact_layout_dump PRIVATE lubancode_core)
target_compile_features(artifact_layout_dump PRIVATE cxx_std_23)

# 0.17.0:集成验证用的刮屏驱动器(AllocConsole + WriteConsoleInput +
# ReadConsoleOutput,真控制台专用),不进 ctest——要真钥匙、要弹控制台
# 窗口,只在集成验证时手动跑:
#   screen_driver <lubancode.exe> <子进程工作目录> <报告文件> [--composer-only]
if(WIN32)
  add_executable(screen_driver EXCLUDE_FROM_ALL screen_driver.cpp)
  target_compile_features(screen_driver PRIVATE cxx_std_23)

  # markdown(0.18.x):markdown 重画专用的刮屏驱动器,同样不进 ctest,
  # 集成验证时手动跑:markdown_screen_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(markdown_screen_driver EXCLUDE_FROM_ALL markdown_screen_driver.cpp)
  target_compile_features(markdown_screen_driver PRIVATE cxx_std_23)

  # bracketed paste:整段多行内容折成一枚占位符，提交时才展开。
  add_executable(paste_screen_driver EXCLUDE_FROM_ALL paste_screen_driver.cpp)
  target_compile_features(paste_screen_driver PRIVATE cxx_std_23)

  # 流式脚注框化(0.22.x):验流式期间正文下方那个跟 composer 视觉一致的框
  # (上横线/输入行/下横线/状态行)、排队回显、ESC 打断、可视区域自然滚屏时
  # 框还贴得住,同样不进 ctest,集成验证时手动跑:
  #   stream_footer_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(stream_footer_driver EXCLUDE_FROM_ALL stream_footer_driver.cpp)
  target_compile_features(stream_footer_driver PRIVATE cxx_std_23)

  # 忙碌排队输入框 Tab 补全(0.30.x):验流式期间排队输入框的候选可见、
  # Tab 当场补全/连按轮转带标记、空正文 Tab no-op、轮末队列泵按 slash
  # 分派(不进 SharedEditor 历史)。要真钥匙,不进 ctest,手动跑:
  #   busy_tab_driver <lubancode.exe> <子进程工作目录> <报告文件>
  add_executable(busy_tab_driver EXCLUDE_FROM_ALL busy_tab_driver.cpp)
  target_compile_features(busy_tab_driver PRIVATE cxx_std_23)

  # /clear、provider 切换清屏、ask_user、diff 残色 + 子代理内层工具展示
  # (重复行/紧凑折叠)专用刮屏驱动器,同样不进 ctest,集成验证时手动跑:
  #   fold_dup_clear_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(fold_dup_clear_driver EXCLUDE_FROM_ALL fold_dup_clear_driver.cpp)
  target_compile_features(fold_dup_clear_driver PRIVATE cxx_std_23)

  # 子代理面板移到输入框上方(0.28.x):验面板在上横线之上、输入框/状态行
  # 钉底、焦点/查看态短标题/两段确认。数据走 LUBANCODE_AGENT_PANEL_DEMO
  # 假代理钩子,不碰网络:
  #   agent_panel_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(agent_panel_driver EXCLUDE_FROM_ALL agent_panel_driver.cpp)
  target_compile_features(agent_panel_driver PRIVATE cxx_std_23)

  # 真前台任务 + 真流式监听(本单新增):进程内起假 anthropic SSE 服务,真
  # lubancode.exe 连上去跑一条带 title 的前台 agent 调用,子代理卡在真工具
  # (ping)里数秒,流式期间逐帧断言代理面板/详情/Esc 分层。工作目录建议给
  # 一个空临时目录(避开项目记忆 identity):
  #   agent_stream_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(agent_stream_driver EXCLUDE_FROM_ALL agent_stream_driver.cpp)
  target_compile_features(agent_stream_driver PRIVATE cxx_std_23)
  target_link_libraries(agent_stream_driver PRIVATE ws2_32)

  # 可视区重排驱动器(多智能体真机回归单):真控制台跑"主回合派两只后台
  # 代理随即收口"的完整回合,断言 composer 顶边/代理坞/状态栏全在可视窗
  # 口里(不许靠改字号/滚轮救场)。五幕:80×24、120×35、全屏、改字号三
  # 时机各十轮、滚动缓冲区非底部;每幕把 viewport 快照写进报告。假
  # anthropic SSE 服务供模型,不碰真网络:
  #   viewport_driver <lubancode.exe> <工作目录> <报告文件>
  add_executable(viewport_driver EXCLUDE_FROM_ALL viewport_driver.cpp)
  target_compile_features(viewport_driver PRIVATE cxx_std_23)
  target_link_libraries(viewport_driver PRIVATE ws2_32)
endif()
