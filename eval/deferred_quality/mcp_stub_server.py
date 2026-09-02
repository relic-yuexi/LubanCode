#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""demosuite:质量对照用的 MCP stdio stub 服务器。

§12.5 十形状的工具环境:30 枚假工具,名字与 schema 按形状设计——
直白/相近易误选(成对成组)/嵌套 object+array+enum/超长命名/明标副作用/
纯干扰池。tools/call 一律回确定性结果(时间/天气回固定值,保证"隔轮再调
核对一致"的任务语义成立),并把入参原样回显,便于判定器核对。

不含任何钥匙;协议为标准 MCP stdio(JSON-RPC 2.0,行分隔 JSON)。
"""

import json
import sys

# ---------------------------------------------------------------- schema 目录

def _desc(purpose: str, detail: str) -> str:
    return f"{purpose}。{detail}"


PADDING = "此工具属于演示套件 demosuite,用于质量对照;描述文字故意写长,撑起延迟挂载的 token 本金,模拟真实 MCP 工具的说明体积。"

TOOLS = [
    {
        "name": "weather_current",
        "description": _desc("查询指定城市此刻的天气", "返回温度、湿度、天气状况与风力。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "城市名,中文或英文均可,如 上海 / Shanghai"},
                "units": {"type": "string", "enum": ["celsius", "fahrenheit"], "description": "温度单位,默认 celsius"},
            },
            "required": ["city"],
            "additionalProperties": False,
        },
    },
    {
        "name": "weather_forecast",
        "description": _desc("查询指定城市未来数日的天气预报", "注意这是预报,不是此刻天气。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "城市名"},
                "days": {"type": "integer", "minimum": 1, "maximum": 7, "description": "预报天数"},
            },
            "required": ["city", "days"],
            "additionalProperties": False,
        },
    },
    {
        "name": "time_now_in_zone",
        "description": _desc("查询指定时区的当前时刻", "timezone 传 IANA 时区名(如 Asia/Shanghai)或 UTC 偏移(如 +08:00)。演示环境返回固定时刻,重复调用结果一致。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "timezone": {"type": "string", "description": "IANA 时区名或 UTC 偏移"},
            },
            "required": ["timezone"],
            "additionalProperties": False,
        },
    },
    {
        "name": "calendar_add_event",
        "description": _desc("向团队日历添加一个单次事件", "只加一场,不重复。start 用 ISO 8601 形如 2026-09-04T10:00:00+08:00。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "title": {"type": "string", "description": "事件标题"},
                "start": {"type": "string", "description": "开始时刻,ISO 8601"},
                "duration_minutes": {"type": "integer", "description": "时长(分钟),如 60 表示一小时"},
            },
            "required": ["title", "start", "duration_minutes"],
            "additionalProperties": False,
        },
    },
    {
        "name": "calendar_add_event_series",
        "description": _desc("向团队日历添加重复事件系列", "按 recurrence 规则周期性重复,适合例会。只要单场事件时不要用这个。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "title": {"type": "string", "description": "事件标题"},
                "start": {"type": "string", "description": "首场开始时刻,ISO 8601"},
                "duration_minutes": {"type": "integer", "description": "每场时长(分钟)"},
                "recurrence": {
                    "type": "object",
                    "description": "重复规则",
                    "properties": {
                        "freq": {"type": "string", "enum": ["daily", "weekly", "monthly"], "description": "重复频率"},
                        "interval": {"type": "integer", "minimum": 1, "description": "间隔数,如 weekly+2 表示每两周"},
                        "count": {"type": "integer", "minimum": 1, "description": "总场次,缺省为无限"},
                    },
                    "required": ["freq", "interval"],
                    "additionalProperties": False,
                },
            },
            "required": ["title", "start", "duration_minutes", "recurrence"],
            "additionalProperties": False,
        },
    },
    {
        "name": "calendar_list_events",
        "description": _desc("列出团队日历的事件", "按时间范围过滤。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "date_from": {"type": "string", "description": "起止日期,ISO 8601 日期"},
                "date_to": {"type": "string", "description": "结束日期"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "calendar_remove_event",
        "description": _desc("从团队日历删除一个事件", "副作用操作,需 event_id。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"event_id": {"type": "string", "description": "事件 id"}},
            "required": ["event_id"],
            "additionalProperties": False,
        },
    },
    {
        "name": "report_build",
        "description": _desc("把若干节组装成一份报告", "sections 数组每节一个:kind=text 放文字,kind=table 放表格(列名进 table.columns),kind=chart 放图表(类型进 chart.chart_type)。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "report_title": {"type": "string", "description": "报告标题,可选"},
                "sections": {
                    "type": "array",
                    "minItems": 1,
                    "description": "报告的节,按顺序排列",
                    "items": {
                        "type": "object",
                        "properties": {
                            "section_title": {"type": "string", "description": "节标题"},
                            "kind": {"type": "string", "enum": ["text", "table", "chart"], "description": "节的种类"},
                            "text_body": {"type": "string", "description": "kind=text 时的正文"},
                            "table": {
                                "type": "object",
                                "description": "kind=table 时的表格",
                                "properties": {
                                    "columns": {"type": "array", "items": {"type": "string"}, "description": "列名"},
                                    "rows": {"type": "array", "items": {"type": "array"}, "description": "数据行"},
                                },
                                "required": ["columns"],
                                "additionalProperties": False,
                            },
                            "chart": {
                                "type": "object",
                                "description": "kind=chart 时的图表",
                                "properties": {
                                    "chart_type": {"type": "string", "enum": ["bar", "line", "pie"], "description": "图表类型"},
                                    "title": {"type": "string", "description": "图表标题"},
                                    "series": {
                                        "type": "array",
                                        "description": "数据系列",
                                        "items": {
                                            "type": "object",
                                            "properties": {
                                                "name": {"type": "string"},
                                                "values": {"type": "array", "items": {"type": "number"}},
                                            },
                                            "required": ["name", "values"],
                                            "additionalProperties": False,
                                        },
                                    },
                                },
                                "required": ["chart_type"],
                                "additionalProperties": False,
                            },
                        },
                        "required": ["section_title", "kind"],
                        "additionalProperties": False,
                    },
                },
            },
            "required": ["sections"],
            "additionalProperties": False,
        },
    },
    {
        "name": "chart_render",
        "description": _desc("单独渲染一张图表", "只画图,不组装报告。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "chart_type": {"type": "string", "enum": ["bar", "line", "pie"], "description": "图表类型"},
                "title": {"type": "string", "description": "图表标题"},
                "data": {
                    "type": "array",
                    "description": "数据点",
                    "items": {
                        "type": "object",
                        "properties": {"label": {"type": "string"}, "value": {"type": "number"}},
                        "required": ["label", "value"],
                        "additionalProperties": False,
                    },
                },
            },
            "required": ["chart_type", "data"],
            "additionalProperties": False,
        },
    },
    {
        "name": "enterprise_datawarehouse_facts_sales_daily_export",
        "description": _desc("从企业数据仓导出每日销售事实表", "长命名工具。lookback_days 指回看天数(1-365),输出 CSV 或 JSON 文件。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "lookback_days": {"type": "integer", "minimum": 1, "maximum": 365, "description": "回看天数,如 30 表示最近 30 天"},
                "format": {"type": "string", "enum": ["csv", "json"], "description": "导出格式,默认 csv"},
                "compress": {"type": "boolean", "description": "是否 gzip 压缩,默认 false"},
            },
            "required": ["lookback_days"],
            "additionalProperties": False,
        },
    },
    {
        "name": "email_send",
        "description": _desc("发送一封邮件", "副作用操作。to 收件地址,subject 主题,body 正文。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "to": {"type": "string", "description": "收件人地址"},
                "subject": {"type": "string", "description": "邮件主题"},
                "body": {"type": "string", "description": "邮件正文"},
                "cc": {"type": "array", "items": {"type": "string"}, "description": "抄送,可选"},
            },
            "required": ["to", "subject", "body"],
            "additionalProperties": False,
        },
    },
    {
        "name": "invoice_create",
        "description": _desc("开具一张正式发票并落账", "副作用操作,开出即生效,不可当草稿用。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "amount": {"type": "number", "description": "金额"},
                "currency": {"type": "string", "enum": ["CNY", "USD", "EUR"], "description": "币种"},
                "note": {"type": "string", "description": "备注,可选"},
            },
            "required": ["amount", "currency"],
            "additionalProperties": False,
        },
    },
    {
        "name": "invoice_create_draft",
        "description": _desc("创建一张草稿发票", "只建草稿,不开正式发票,可后续修改。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "amount": {"type": "number", "description": "金额"},
                "currency": {"type": "string", "enum": ["CNY", "USD", "EUR"], "description": "币种"},
                "note": {"type": "string", "description": "备注,可选"},
            },
            "required": ["amount", "currency"],
            "additionalProperties": False,
        },
    },
    {
        "name": "invoice_list",
        "description": _desc("列出发票", "按状态过滤。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "status": {"type": "string", "enum": ["draft", "issued", "paid"], "description": "状态过滤,可选"},
                "limit": {"type": "integer", "minimum": 1, "maximum": 100, "description": "条数上限"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "invoice_send_by_email",
        "description": _desc("把一张已开发票通过邮件发给收件人", "副作用操作,需已存在的 invoice_id。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "invoice_id": {"type": "string", "description": "发票 id"},
                "to": {"type": "string", "description": "收件人地址"},
            },
            "required": ["invoice_id", "to"],
            "additionalProperties": False,
        },
    },
    {
        "name": "server_restart",
        "description": _desc("重启指定环境里的一个服务", "副作用操作。service 服务名,environment 环境名。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "service": {"type": "string", "description": "服务名,如 payments"},
                "environment": {"type": "string", "enum": ["demo", "staging", "prod"], "description": "环境"},
                "reason": {"type": "string", "description": "重启原因,可选"},
            },
            "required": ["service", "environment"],
            "additionalProperties": False,
        },
    },
    {
        "name": "server_restart_all",
        "description": _desc("重启指定环境里的全部服务", "副作用操作,影响面大,除非明确要求不要用。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "environment": {"type": "string", "enum": ["demo", "staging", "prod"], "description": "环境"},
                "confirm": {"type": "boolean", "description": "必须显式传 true"},
            },
            "required": ["environment", "confirm"],
            "additionalProperties": False,
        },
    },
    {
        "name": "deployment_launch",
        "description": _desc("发起一次部署", "副作用操作。resources 指定 CPU/内存/副本数,tags 打标。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "部署名"},
                "environment": {"type": "string", "enum": ["demo", "staging", "prod"], "description": "环境"},
                "resources": {
                    "type": "object",
                    "description": "资源规格",
                    "properties": {
                        "cpu_cores": {"type": "number", "description": "CPU 核数"},
                        "memory_mb": {"type": "integer", "description": "内存 MB"},
                        "replicas": {"type": "integer", "minimum": 1, "description": "副本数"},
                    },
                    "required": ["cpu_cores", "memory_mb", "replicas"],
                    "additionalProperties": False,
                },
                "tags": {"type": "array", "items": {"type": "string"}, "description": "标签,可选"},
            },
            "required": ["name", "environment", "resources"],
            "additionalProperties": False,
        },
    },
    {
        "name": "deployment_rollback",
        "description": _desc("回滚一次部署到指定修订版", "副作用操作。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "deployment_id": {"type": "string", "description": "部署 id"},
                "to_revision": {"type": "integer", "description": "目标修订版号"},
            },
            "required": ["deployment_id", "to_revision"],
            "additionalProperties": False,
        },
    },
    {
        "name": "dashboard_refresh",
        "description": _desc("刷新一个仪表盘的数据", "副作用操作。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"dashboard_id": {"type": "string", "description": "仪表盘 id"}},
            "required": ["dashboard_id"],
            "additionalProperties": False,
        },
    },
    {
        "name": "file_checksum_sha1",
        "description": _desc("计算文件的 SHA-1 校验和", "返回 40 位十六进制。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "文件路径"}},
            "required": ["path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "file_checksum_sha256",
        "description": _desc("计算文件的 SHA-256 校验和", "返回 64 位十六进制。注意与 SHA-1 是不同算法。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "文件路径"}},
            "required": ["path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "db_query_readonly",
        "description": _desc("以只读连接执行 SQL 查询", "只读,不能改数据。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "sql": {"type": "string", "description": "SQL 语句"},
                "max_rows": {"type": "integer", "minimum": 1, "maximum": 1000, "description": "行数上限,可选"},
            },
            "required": ["sql"],
            "additionalProperties": False,
        },
    },
    {
        "name": "db_query_execute",
        "description": _desc("以可写连接执行 SQL", "副作用操作,可改数据,谨慎使用。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"sql": {"type": "string", "description": "SQL 语句"},
            },
            "required": ["sql"],
            "additionalProperties": False,
        },
    },
    {
        "name": "kv_get",
        "description": _desc("读取键值存储里的一个键", "只读。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"key": {"type": "string", "description": "键名"}},
            "required": ["key"],
            "additionalProperties": False,
        },
    },
    {
        "name": "kv_put",
        "description": _desc("写入键值存储的一个键", "副作用操作。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "键名"},
                "value": {"type": "string", "description": "值"},
            },
            "required": ["key", "value"],
            "additionalProperties": False,
        },
    },
    {
        "name": "kv_delete",
        "description": _desc("删除键值存储的一个键", "副作用操作。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {"key": {"type": "string", "description": "键名"}},
            "required": ["key"],
            "additionalProperties": False,
        },
    },
    {
        "name": "metrics_query",
        "description": _desc("查询一个指标在时间窗内的聚合值", "aggregation 选聚合方式。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "metric": {"type": "string", "description": "指标名,如 cpu.usage"},
                "window_minutes": {"type": "integer", "minimum": 1, "description": "时间窗(分钟)"},
                "aggregation": {"type": "string", "enum": ["sum", "avg", "max", "min"], "description": "聚合方式"},
            },
            "required": ["metric", "window_minutes", "aggregation"],
            "additionalProperties": False,
        },
    },
    {
        "name": "log_search",
        "description": _desc("按关键词搜索服务日志", "level 过滤级别。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "关键词"},
                "level": {"type": "string", "enum": ["debug", "info", "warn", "error"], "description": "级别过滤,可选"},
                "limit": {"type": "integer", "minimum": 1, "maximum": 500, "description": "条数上限,可选"},
            },
            "required": ["query"],
            "additionalProperties": False,
        },
    },
    {
        "name": "log_tail",
        "description": _desc("取一个服务最近的日志尾部", "只读。" + PADDING),
        "inputSchema": {
            "type": "object",
            "properties": {
                "service": {"type": "string", "description": "服务名"},
                "lines": {"type": "integer", "minimum": 1, "maximum": 1000, "description": "行数,可选"},
            },
            "required": ["service"],
            "additionalProperties": False,
        },
    },
]

TOOL_NAMES = {t["name"] for t in TOOLS}

# ---------------------------------------------------------------- 调用响应

FIXED_TIME = "2026-09-03T14:00:00+08:00"


def call_tool(name: str, args: dict) -> dict:
    """确定性响应:关键工具回固定值(时间/天气),其余回执+参数回显。"""
    if name == "weather_current":
        return {
            "city": args.get("city"),
            "temperature_c": 23,
            "condition": "多云",
            "humidity_pct": 62,
            "wind_kph": 11,
            "observed_at": FIXED_TIME,
            "note": "演示环境固定天气数据",
        }
    if name == "weather_forecast":
        return {"city": args.get("city"), "days": args.get("days"), "forecast": "演示预报数据(未启用)"}
    if name == "time_now_in_zone":
        return {"timezone": args.get("timezone"), "datetime": FIXED_TIME, "utc_offset": "+08:00", "note": "演示环境固定时刻,重复调用一致"}
    if name == "calendar_add_event":
        return {"status": "created", "event_id": "evt_0001", "echo": args, "recurring": False}
    if name == "calendar_add_event_series":
        return {"status": "series_created", "series_id": "ser_0001", "echo": args, "recurring": True}
    if name == "calendar_list_events":
        return {"events": [], "echo": args}
    if name == "calendar_remove_event":
        return {"status": "removed", "echo": args}
    if name == "report_build":
        return {"status": "built", "section_count": len(args.get("sections", [])), "echo": args}
    if name == "chart_render":
        return {"status": "rendered", "echo": args}
    if name == "enterprise_datawarehouse_facts_sales_daily_export":
        return {
            "status": "completed",
            "file": "facts_sales_daily_last%dd.csv" % args.get("lookback_days", 0),
            "rows": 214590,
            "bytes": 9451230,
            "echo": args,
        }
    if name == "email_send":
        return {"status": "queued", "message_id": "msg_0001", "to": args.get("to"), "subject": args.get("subject")}
    if name == "invoice_create":
        return {"status": "issued", "invoice_id": "inv_0001", "echo": args}
    if name == "invoice_create_draft":
        return {"status": "draft_created", "draft_id": "dft_0001", "echo": args}
    if name == "invoice_list":
        return {"invoices": [], "echo": args}
    if name == "invoice_send_by_email":
        return {"status": "sent", "echo": args}
    if name == "server_restart":
        return {"status": "restarted", "service": args.get("service"), "environment": args.get("environment"), "took_seconds": 6}
    if name == "server_restart_all":
        return {"status": "restarted_all", "environment": args.get("environment"), "affected": 12}
    if name == "deployment_launch":
        return {"status": "launched", "deployment_id": "dep_0001", "echo": args}
    if name == "deployment_rollback":
        return {"status": "rolled_back", "echo": args}
    if name == "dashboard_refresh":
        return {"status": "refreshed", "echo": args}
    if name == "file_checksum_sha1":
        return {"path": args.get("path"), "algorithm": "sha1", "hex": "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4"}
    if name == "file_checksum_sha256":
        return {
            "path": args.get("path"),
            "algorithm": "sha256",
            "hex": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
        }
    if name == "db_query_readonly":
        return {"rows": [], "echo": args}
    if name == "db_query_execute":
        return {"affected_rows": 0, "echo": args}
    if name == "kv_get":
        return {"key": args.get("key"), "value": None}
    if name == "kv_put":
        return {"status": "stored", "echo": args}
    if name == "kv_delete":
        return {"status": "deleted", "echo": args}
    if name == "metrics_query":
        return {"metric": args.get("metric"), "value": 41.7, "window_minutes": args.get("window_minutes")}
    if name == "log_search":
        return {"matches": [], "echo": args}
    if name == "log_tail":
        return {"lines": ["(演示日志尾行)"], "echo": args}
    return {"status": "unknown_tool", "name": name}


# ---------------------------------------------------------------- MCP stdio 协议

PROTOCOL_VERSION = "2024-11-05"


def handle(msg: dict) -> dict | None:
    method = msg.get("method", "")
    msg_id = msg.get("id")
    is_request = msg_id is not None
    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "demosuite", "version": "1.0.0"},
            },
        }
    if method in ("notifications/initialized", "initialized"):
        return None  # 通知不回
    if method == "ping":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {"tools": TOOLS}}
    if method == "tools/call":
        params = msg.get("params", {})
        name = params.get("name", "")
        args = params.get("arguments", {}) or {}
        if name not in TOOL_NAMES:
            return {
                "jsonrpc": "2.0",
                "id": msg_id,
                "result": {"content": [{"type": "text", "text": json.dumps({"error": "unknown_tool", "name": name}, ensure_ascii=False)}], "isError": True},
            }
        result = call_tool(name, args)
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {"content": [{"type": "text", "text": json.dumps(result, ensure_ascii=False)}], "isError": False},
        }
    if is_request:
        return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32601, "message": "method not found: %s" % method}}
    return None


def main() -> int:
    # Windows 子进程默认 codepage 多为 GBK,不重配的话写中文即 UnicodeEncodeError,
    # client 端只看得见进程悄悄死掉,报出来的却是 tools/list 超时。
    try:
        sys.stdin.reconfigure(encoding="utf-8")
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        reply = handle(msg)
        if reply is not None:
            sys.stdout.write(json.dumps(reply, ensure_ascii=False) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
