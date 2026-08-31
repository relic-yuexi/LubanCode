package com.lubancode.console;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.webkit.WebSettings;
import android.webkit.WebView;

/**
 * LubanCode 参考前端的 Android WebView 壳(多前端外壳单·阶段 E)。
 *
 * 壳只做三件事:全屏 WebView、开 JS、装 assets 里的参考前端
 * (file:///android_asset/index.html——Gradle 的 assets.srcDir 直指
 * examples/web-console,与浏览器页同一份代码)。协议、手势折输入
 * (web_console_touch.js)全在前端 JS 里;壳不为内核加一行,内核也不知
 * 壳存在。连接走 adb reverse tcp:8765 tcp:8765 后页顶填 127.0.0.1 可达
 * 的端口(首版本机反向代理口径,见 examples/shells/README.md)。
 */
public final class MainActivity extends Activity {

    private WebView web;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        web = new WebView(this);
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(true); // 从 file:///android_asset 装参考前端
        web.setBackgroundColor(0xFF10141C);
        setContentView(web);
        web.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        web.loadUrl("file:///android_asset/index.html");
    }

    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) {
            web.goBack(); // 页内历史先退,壳不抢
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
