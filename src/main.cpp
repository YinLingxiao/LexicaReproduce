// 程序入口：读取启动参数，打开数据目录，再进入网页服务或控制台菜单。
#include "api/server.h"
#include "app/app.h"
#include "cli/cli.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

// 取得当前 exe 所在目录，作为默认数据目录和网页目录的起点。
fs::path exeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return fs::current_path();
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
}

}

// 读取命令行参数，启动 App；启动失败则退出，否则进入用户选择的界面。
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    fs::path base = exeDir();
    fs::path dataDir = base / "data";
    fs::path webDir = base / "web";
    int port = 8765;
    bool cli = false;
    // 启动参数可覆盖默认目录、端口，--cli 表示使用控制台菜单。
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cli") cli = true;
        else if (arg == "--data-dir" && i + 1 < argc) dataDir = fs::path(argv[++i]);
        else if (arg == "--web-dir" && i + 1 < argc) webDir = fs::path(argv[++i]);
        else if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else {
            std::cerr << "BAD_REQUEST " << arg << "\n";
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "BAD_REQUEST --port\n";
        return 2;
    }
    // 先完成数据目录和数据库检查，再开放界面供用户操作。
    App app;
    StartupStatus status = app.start(dataDir.u8string());
    std::cout << startupName(status) << "\n";
    if (status == StartupStatus::DirNotWritable || status == StartupStatus::DbCorrupt || status == StartupStatus::VersionTooNew) {
        return 1;
    }
    int code = 0;
    // 网页模式会持续监听请求，直到用户结束程序。
    if (cli) {
        code = runCli(app);
    } else {
        std::cout << "http://127.0.0.1:" << port << "\n";
        code = runServer(app, port, webDir.u8string());
    }
    app.shutdown();
    return code;
}
