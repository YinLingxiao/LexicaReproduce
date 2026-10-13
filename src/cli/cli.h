// 控制台界面：用数字菜单调用 App，便于直接验证各项功能。
#pragma once

class App;

// 循环显示数字菜单，读取选择后调用相应业务功能。
int runCli(App& app);
