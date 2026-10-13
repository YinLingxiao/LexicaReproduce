// 网页接口：接收 HTTP 请求，检查参数，调用 App，再把结果转成 JSON。
#pragma once
#include <string>

class App;

// 设置超时与请求大小限制，只在 127.0.0.1 上启动服务。
int runServer(App& app, int port, const std::string& webDir);
void requestServerStop();
