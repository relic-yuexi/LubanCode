// LubanCode 参考前端的 Tauri 桌面壳(多前端外壳单·阶段 E)。
//
// 全部活计在 tauri.conf.json:窗口尺寸、前端指向(../../web-console,
// 不复制)、CSP(给 WS 与 artifact 字节口子开回环的口)。这文件只是
// 标准 Tauri 引导——壳不为内核加一行,内核也不知壳存在。
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    tauri::Builder::default()
        .run(tauri::generate_context!())
        .expect("Tauri 壳没能起窗");
}
