#include "ui/window.h"
#ifndef DECLSPEC_XFGVIRT
#define DECLSPEC_XFGVIRT(base, func)
#endif
#include "webview.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

int runWindow(int port) {
    webview::webview window(false, nullptr);
    if (!window.window()) {
        MessageBoxW(nullptr, L"未找到 WebView2 运行时", L"Lexica", MB_ICONERROR);
        return 1;
    }
    window.set_title("Lexica");
    window.set_size(1200, 760, WEBVIEW_HINT_NONE);
    window.set_size(960, 640, WEBVIEW_HINT_MIN);
    window.navigate("http://127.0.0.1:" + std::to_string(port) + "/");
    window.run();
    return 0;
}
