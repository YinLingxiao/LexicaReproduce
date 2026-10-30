# Lexica

Windows 本地英语字典，使用 C++17 后端、WebView2 桌面壳和 Svelte 5 界面。以离线查词为核心，支持单词和例句发音、个人词库、笔记、复习、统计，以及可选 AI 例句。界面为英文，保留中文释义和翻译。

已发布的历史程序包：[0.1.0 Release](https://github.com/YinLingxiao/LexicaReproduce/releases/tag/v0.1.0)，使用说明见 [RELEASE-0.1.0.md](docs/parity/RELEASE-0.1.0.md)。该包早于本次发音、启动和英文界面修订；当前源码请按下方命令构建。

## C++ / WebView2 复刻版

当前实现位于 `src/parity` 和 `ui`，已合并到 `main`。接口合同为 1.3.0（34 个命令），协议 1，SQLite schema 6。界面方向见 [UI_DIRECTION.md](docs/parity/UI_DIRECTION.md)，构建和架构见 [工程入口](docs/parity/README.md)，测试结果及限制见 [验收记录](docs/parity/VALIDATION.md)。

需要 Windows x64、Visual Studio 2022 桌面 C++ 工具、Windows SDK、CMake 3.21+、Node/npm，以及 WebView2 Runtime。在 PowerShell 中运行：

```powershell
./tools/bootstrap.ps1
./tools/build-phase0.ps1 -Smoke
./tools/package.ps1 -OutputDirectory dist/LexicaReproduce-English
./dist/LexicaReproduce-English/lexica_desktop.exe
```

安装程序只需完整分发目录和 WebView2 Runtime。用户数据存储在 `%LOCALAPPDATA%/LexicaReproduce/parity-v1`，不随源码上传。MSVC 构建支持已安装的 Windows 英语音色；本机缺少目标口音时会标明实际音色。UCRT 构建不支持系统 TTS。

| 当前目录 | 职责 |
|---|---|
| `src/parity/app` | 应用编排、命令执行和后台任务 |
| `src/parity/modules` | 字典、词库、复习、统计、AI 和 TTS |
| `src/parity/desktop` | Win32 / WebView2 桌面壳 |
| `src/parity/storage` | SQLite 存储和迁移 |
| `contracts` | C++ / TypeScript 共用接口与模型 |
| `ui/src` | Svelte 界面、交互和发音控制器 |
| `tests/parity`、`ui/tests` | 后端、界面和真实服务回归 |
| `tools` | 依赖初始化、构建、打包和验证脚本 |

前端回归在 `ui` 目录按顺序运行（TTS 控制器测试使用 Node 22.6+ 的类型剥离支持，浏览器测试使用 Microsoft Edge）：

```powershell
npm run check
npm run test:tts
npm run build
npm run test:e2e:fixture
npm run test:home
npm run test:live
npm run test:e2e
```

真实服务测试默认使用已构建的 MSVC Debug 服务和独立测试数据库；可通过 `LEXICA_SERVER_EXE`、`LEXICA_UI_DIR` 验证其他构建。浏览器测试截图写入已忽略的 `ui/test-results`，人工选定的验收截图保存在 `docs/parity/evidence`。静态构建和开发服务器共用 SvelteKit 输出目录，请顺序执行。

历史分工和审核可见 [六 Agent 任务](docs/agents/README.md) 与 [集成审核报告](docs/parity/REVIEW.md)。以下内容仅适用于保留的旧版实现，使用独立的入口和数据目录。

## 旧版实现：源码入口

先看 [程序入口](src/main.cpp)，再看 [App 业务编排](src/app/app.cpp)。App 展示一次查词、一次复习如何调用其他模块；具体实现继续进入对应模块。

| 目录 | 职责 |
|---|---|
| `src/common` | 共用类型、错误码、时间和文本工具 |
| `src/dict` | 词库查询、CSV/SQLite 导入、共享数据库的建表升级 |
| `src/vocab` | 收藏、笔记、暂停、查词历史和活动统计 |
| `src/review` | 到期选题、提示、判分、错词重练和成绩保存 |
| `src/ai` | AI 配置、连接测试、后台例句任务和缓存 |
| `src/app` | 协调以上业务模块，提供界面调用入口 |
| `src/api`、`src/cli` | 网页 HTTP 接口与控制台菜单 |
| `web` | 旧版五页面网页资源，与新版 `ui` 分开 |
| `tests`、`tests/data` | 验收测试及只读测试词库 |
| `third_party` | SQLite、cpp-httplib、nlohmann/json |
| `docs` | 运行、接口、需求和模块说明；旧分工仅保存在 `docs/history` |

模块保持独立的 CMake 库目标。接口与控制台通过 App 调用业务；复习依赖词库和个人词汇；个人词汇通过公共的 `StageSource` 接口读取阶段，不反向依赖复习实现。

## 旧版实现：构建和运行

使用 Windows x64、MSYS2 UCRT64、C++17。需要的 MSYS2 软件包：

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

在项目根目录运行：

```bat
build.bat
build\lexica.exe
```

浏览器打开 `http://127.0.0.1:8765`。控制台模式用 `build\lexica.exe --cli`。网页会复制到 exe 旁边的 `web` 目录，默认用户数据放在 exe 旁边的 `data` 目录；已有数据不随源码迁移。

更多启动参数、AI 配置和备份方法见 [运行说明](docs/运行说明.md)，HTTP 数据格式见 [接口说明](docs/接口说明.md)。VS Code 的调试入口指向 `build/lexica.exe`，调试前先构建。

## 旧版实现：测试

默认同时构建四个测试程序，编译完成后运行：

```bat
ctest --test-dir build --output-on-failure
```

`dict_test` 验证词库导入，`vocab_tests` 验证个人词汇和统计，`review_test` 验证复习与应用联调，`import_concurrency_test` 验证词条和词形暂存期间查词、收藏、笔记仍可写入。测试链接同一份业务库，不需要手工编译源码。也可以从任意工作目录直接运行构建目录里的测试 exe。

测试词库使用 CMake 配置的绝对路径。可写数据位于 `<构建目录>/test-output/<模块>/<进程编号>`，不会写进用户的 `data` 或修改样本；目录清理会先检查路径边界。

只需要主程序时，在 UCRT64 环境中配置 `cmake -S . -B build -G Ninja -DBUILD_TESTING=OFF`。需要恢复测试时重新配置 `-DBUILD_TESTING=ON`。采用其他构建目录时，程序、网页和测试输出也放在该目录下。

## 旧版实现：模块说明与已知问题

- [词库与导入](docs/modules/dict.md)
- [个人词汇与统计](docs/modules/vocab.md)
- [复习与记忆调度](docs/modules/review.md)

目录整理保留现有接口、数据库结构和业务行为。后台导入暂存期间不再提前取得共享库写锁；最后原子应用正式词库时仍需短暂取得写锁，其他写操作可能等待。前端完整业务页面仍需另行交付。
