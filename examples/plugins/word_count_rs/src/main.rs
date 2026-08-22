//! word_count_tool — Rust process 插件示例(plugins 单第 9 步)。
//!
//! 协议 v1:stdin 恰好一份 JSON,stdout 恰好一份 JSON,退出即结束。
//! Rust 编出的可执行文件自带 runtime,用户机器零依赖——这正是 process
//! 插件对 Rust 作者的意义:不必装 rustc 也能用。

use std::io::{self, Read, Write};

fn main() {
    let mut input = String::new();
    if io::stdin().read_to_string(&mut input).is_err() {
        eprintln!("读 stdin 失败");
        std::process::exit(1);
    }
    let request: serde_json::Value = match serde_json::from_str(&input) {
        Ok(value) => value,
        Err(error) => {
            // 请求帧都读不进来:唯一能做的是回一份失败帧(call_id 只能空)。
            let response = serde_json::json!({
                "protocol": 1, "call_id": "", "ok": false,
                "error": {"code": "bad_request", "message": error.to_string()},
            });
            let _ = io::stdout().write_all(response.to_string().as_bytes());
            return;
        }
    };
    let call_id = request["call_id"].as_str().unwrap_or("").to_string();
    if request["tool"].as_str() != Some("word_count") {
        let response = serde_json::json!({
            "protocol": 1, "call_id": call_id, "ok": false,
            "error": {"code": "unknown_tool", "message": "不认得的工具"},
        });
        let _ = io::stdout().write_all(response.to_string().as_bytes());
        return;
    }
    let text = request["arguments"]["text"].as_str().unwrap_or("");
    let count = text.split_whitespace().count();
    let response = serde_json::json!({
        "protocol": 1, "call_id": call_id, "ok": true,
        "content": [{"type": "text", "text": format!("词数: {count}")}],
        "structured": count,
    });
    let _ = io::stdout().write_all(response.to_string().as_bytes());
}
