---
name: fact.build-cmd-001
description: "demo-repo 用 CMake 构建,测试跑 ctest -C Release,全量约三分钟"
metadata:
  schema: 3
  node_type: memory
  type: fact
  id: fact.build-cmd-001
  confidence: verified
  status: active
  scope:
    level: project
    kind: project
    value: ""
  origin_session_ids:
    - "20260116-100003-tool-roundtrip"
  created: "2026-01-16T10:01:00Z"
  modified: "2026-01-16T10:01:00Z"
  last_verified: "2026-01-16T10:01:00Z"
  expires: null
  keywords:
    - cmake
    - ctest
    - release
  evidence:
    - path: CMakeLists.txt
      symbol: ""
fingerprints:
  CMakeLists.txt: "aa11bb22cc33dd44ee55ff6677889900aabbccdd"
---

# demo-repo 构建与测试命令

构建入口在仓库顶层 CMakeLists.txt。configure 之后直接 build,测试统一
跑 `ctest -C Release`,全量约三分钟,单册秒级。
