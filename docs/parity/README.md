# Phase 0 公共基座与交接入口

当前界面方向以用户最新确认的 [UI_DIRECTION.md](UI_DIRECTION.md) 为准：字典优先、英文界面、小喇叭发音，搜索页不放宣传语或复习任务卡。下文保留历史工程记录。

六个模块与 TTS 已合并到 `main`，34 个命令均已注册。最新验收见 [VALIDATION.md](VALIDATION.md)，发音集成见 [INTEGRATION_TTS_HOME_REVIEW.md](INTEGRATION_TTS_HOME_REVIEW.md)，启动修复见 [STARTUP_LOOKUP_FIX.md](STARTUP_LOOKUP_FIX.md)。2026-10-27 的对照验收及六 Agent 审核分别保留在 [ACCEPTANCE.md](ACCEPTANCE.md) 和 [REVIEW.md](REVIEW.md)。

目标：Lexica 0.2.0，参考 SHA `c4b92a39402860566bf954f92efc8e59666aaa4a`。
公共合同版本 `1.3.0`；协议版本 `1`；SQLite `user_version=6`。1.2.0 增加词频及等级来源，1.3.0 增加系统发音接口；0006 迁移只追加来源列，不重写 0001～0005。

用户已确认 C++ 后端和桌面壳，保留原版 Svelte 网页技术，优先外观与交互 1:1。

## 先读什么

1. 本文：构建、现状、目录和并行边界。
2. [公共接口合同](CONTRACT.md)：请求、DTO、数据库、锁、错误与事件。
3. [源码映射与兼容决定](SOURCE_MAP.md)：每个模块的参考源码与已知原版边界。
4. [Agent 提示词入口](../agents/README.md)：A～F 可复制的完整任务及交付模板。
5. [验收记录](VALIDATION.md)：本次实际执行的结果与未覆盖部分。

## 冻结的技术与目录

- Windows x64、C++17、MSVC 2022、Windows SDK、CMake 3.21+。
- WebView2 SDK 固定版本和包哈希见 `tools/dependencies.json`；生产机器需要 WebView2 Runtime。
- 前端 Svelte 5 + TypeScript + Vite，使用 `ui/package-lock.json` 锁定依赖。
- 新代码统一位于 `src/parity/`，命名空间 `lexica::parity`；旧 `src/app`、`src/dict` 等实现仍供旧程序与旧测试使用。
- 新版数据库与原应用、旧 C++ 程序分开。身份根目录是 `%LOCALAPPDATA%/LexicaReproduce/parity-v1`。`db_dir.txt` 里写绝对目录时，只有 `lexica.db` 和词库下载放到那个目录；WebView2 profile 仍在身份根的 `webview-profile`。凭据服务名是 `LexicaReproduce.parity-v1`，账号 `ai_api_key`，不使用原版 `com.lexica.app`。指向原版数据目录的 `db_dir.txt` 会被拒绝。
- 实际目录采用独立 `src/parity`，替代早期方案中的分散建议目录，以隔离旧行为并明确写入所有权。

## 构建和启动

在 PowerShell 中进入仓库根目录，确保 Node/npm 和 CMake 在 PATH，安装 Visual Studio 2022 的桌面 C++ 工具及 Windows SDK：

```powershell
./tools/bootstrap.ps1
./tools/build-phase0.ps1 -Smoke
```

`bootstrap.ps1` 校验下载的 WebView2 SDK 包哈希，安装锁定的前端依赖；`build-phase0.ps1` 检查生成合同、检查/构建 UI、构建 C++、运行 CTest、复制资源，并可选执行真实 WebView2 冒烟验证。每次冒烟使用独立临时数据库并生成报告/截图。

开发桌面程序：

```powershell
./build-parity-msvc/src/parity/Debug/lexica_desktop.exe
```

默认窗口内容为 1180×820，最小内容为 480×400，支持每显示器 DPI。关闭窗口会停止分发、等待命令工作线程并释放控制器与数据库。

开发浏览器模式使用相同 C++ Router：

```powershell
./build-parity-msvc/src/parity/Debug/lexica_parity_server.exe --port 8766
# 浏览器访问 http://127.0.0.1:8766
# UI 热更新：另一个终端进入 ui，执行 npm run dev
```

也可用 `lexica_parity_server.exe --stdio --data-dir <测试目录>`，逐行输入 JSON 请求、逐行读取 JSON 响应。测试数据使用绝对路径和独立目录。

`parity-ucrt` 预设作为非桌面核心的兼容构建路径；统一交付和 Agent 验收使用 `parity-msvc`。旧 `build.bat` 仍提供原程序构建入口。

生产包不携带 Node、Rust 或 Vite：

```powershell
./tools/check-runtime.ps1
./tools/package.ps1
./tools/install.ps1 -Source .\dist\LexicaReproduce
./tools/uninstall.ps1
```

安装目录是 `%LOCALAPPDATA%/LexicaReproduce/app`。安装器保存文件所有权清单，升级替换清单中的文件，卸载只删除清单中的文件及空目录，保留自行放入的文件。非空且没有安装清单的目录会被拒绝。个人数据 `parity-v1` 默认保留；只有 `-DeleteData` 才删除该固定目录内的个人库、profile 和 `db_dir.txt`，外部 db_dir 指向的数据目录不自动删除。

Release 使用静态 MSVC 运行库，分发仍需要 WebView2 Runtime。打包会重新构建当前代码和 UI，不沿用目录里已有的旧程序。可运行 `tools/test-install.ps1 -Package <包目录绝对路径>` 验证安装、升级、卸载及非本程序文件保护；测试不会执行 `-DeleteData`。

## Phase 0 的完成范围

- 已迁移原版五个页面、组件、字体和静态资源，集中替换 Tauri 传输/事件/外链接口。
- 公共合同登记原版全部 31 个命令；生成 C++ 模型、请求类型、序列化和 TypeScript 模型/命令映射。
- 已实现 `app_info`、种子词前缀建议、只读种子词查询及命中/未命中结果。
- SQLite 应用原版五次迁移，开启 FTS5、外键、WAL 和 busy timeout，幂等导入固定源码中的 55 个种子词。
- 原生宿主的消息桥与 HTTP/stdio 共用命令分发和校验。原生消息仅接收应用来源；请求在后台分发，DB 临界区串行化。
- `system_capabilities` 是新增的基座诊断命令，显示逐命令可用性和实现程度。未注册的业务明确返回 `NOT_IMPLEMENTED`。

查询目前**不产生 encounters 或 memory_states**，词库信息仍如实返回 `dictionary_ready=false`；种子查询不代表完整查词语义完成。C 接入查词事件与调度后才能标为业务实现。完整 ECDICT、学习、收藏/笔记、统计和 AI 留给后续模块。

自动完整词库初始化仅在 `ensure_dictionary` 真实注册后启用。Phase 0 不会在用户打开程序时自动下载大词库，也不会把种子库冒充完整 ECDICT。

## 并行边界

| 所有者 | 可写目录 | 公共入口 |
|---|---|---|
| A | contracts、src/parity/contracts/storage/api/app/desktop、tools、根构建与交接文档 | 模型、迁移、Router、宿主及集成 |
| B | src/parity/modules/dictionary、tests/parity/dictionary | makeDictionary / registerDictionary |
| C | src/parity/modules/learning、tests/parity/learning | registerLearning，包括 lookup_word 的事件编排 |
| D | src/parity/modules/library、tests/parity/library | registerLibrary |
| E | ui，排除 generated、api.ts、transport.ts 的公共合同改动 | 原版页面、组件、偏好与页面测试 |
| F | src/parity/modules/ai、tests/parity/ai | registerAi |

模块内的 `CMakeLists.txt` 可添加自身源文件/测试，避免多人修改根构建清单。需要新增公共字段、迁移、链接库或跨模块端口时，提交变更需求给 A；不要在模块目录内复制另一套公共类型。

所有模块都能立即从当前基座编译：B/D/F 可独立实施；C 可用 DictionaryPort 夹具并行；E 使用当前真实只读查询或明确标记的开发夹具实现页面测试。每个 Agent 使用独立 worktree 和测试目录。

## 依赖与许可

原版 MIT 许可、Inter 和 Newsreader 字体 OFL 许可保存在 `third_party/licenses/`；分发时保留。SDK 和 npm 包自身许可随对应依赖保留。依赖审计和未解决项见验收记录。
