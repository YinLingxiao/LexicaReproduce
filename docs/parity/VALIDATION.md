# Phase 0 验收记录

日期：2026-10-26。参考提交 c4b92a39402860566bf954f92efc8e59666aaa4a；合同 1.0.0；协议 1；schema 5。

## 实际完成

| 验证 | 结果 | 证明范围 |
|---|---|---|
| 生成合同 `node tools/generate-contracts.mjs --check` | 通过 | C++/TS/目录生成物与权威 JSON 一致 |
| 固定资源 `node tools/verify-baseline.mjs` | 10 个文件通过 | 原版 API、种子、五次迁移和字体；文本换行归一化 |
| MSVC x64 `cmake --preset parity-msvc` / build | 通过 | 旧程序、新核心、HTTP/stdio、WebView2 宿主均可构建 |
| MSVC `ctest --preset parity-msvc` | 5/5 通过 | 原有四项回归 + 新基座测试 |
| UCRT64 `parity-ucrt` build / ctest | 构建通过，5/5 通过 | 非桌面核心及旧程序的原工具链兼容性 |
| `npm run check` | 0 错误、0 警告 | Svelte/TypeScript 类型 |
| `npm run build` | 通过 | 静态生产 UI、字体和 bootstrap CSP 生成 |
| `node tools/smoke-rpc.mjs` | 11 项验证通过 | 实际 HTTP→Router→SQLite；命中/未命中/建议/非法参数/未实现/来源/类型/SPA/中文路径 |
| `npm run test:e2e` | 1 项端到端场景通过 | 已构建生产 Svelte + 实际 C++：建议键盘选词、词条/例句、设置导航、480px、无未处理页面异常 |
| WebView2 `--smoke-report` | ok=true、ui_rendered=true | 原生窗口加载生产资源、消息桥、真实 SQLite 查询、错误返回及截图 |

环境：MSVC 19.44.35221、Windows SDK 10.0.26100.0；UCRT64 GCC 16.2.0；Node 24.15.0；实际 WebView2 Runtime 155.0.4283.62。依赖以锁文件为准，环境版本记录不代表跨机器性能承诺。

## 基础测试覆盖

parity_foundation_test 验证 schema/FTS/外键、55 条种子幂等和稳定 ID、嵌套 DTO、可空值、命中/未命中、默认参数和边界校验、非法枚举/协议/未知命令、未实现写入失败、事务回滚、v2→v5 迁移保留事实、新库版本拒绝、旧库隔离、中文数据库路径、毫秒时间往返与非法日期。

现有四项为 dict_test、vocab_tests、review_test、import_concurrency_test；这些仍验证旧业务，不作为新版学习算法已经完成的证据。

## 证据位置和重复执行

- C++ 测试结果：各构建目录的 Testing/Temporary/LastTest.log。
- HTTP 报告：build-parity-msvc/test-output/rpc 下的独立运行目录。
- Playwright 图片：ui/test-results 下的 lookup.png。
- 原生报告/图片：build-parity-msvc/test-output/desktop 下的 report.json / desktop.png。
- 成功的原生样本快照另外保存于本目录 evidence，供快速查看；原始测试输出不进入用户数据库。

执行 `tools/build-phase0.ps1 -Smoke` 可重复构建/CTest/原生冒烟；HTTP 和浏览器测试分别使用上表命令。Agent 在各自 worktree 执行并保留自己的结果。

## 修复的基础链路问题

- 原版 seed.json 为 55 条，修正沿用 README 的 50 条测试预期。
- 原版 practice_check 的 Rust bool 与前端对象类型不一致，在 API 适配层显式修正。
- 新版服务器使用 UTF-16 Windows 命令行、UTF-8 源码路径转换，验证中文目录。
- 桌面通过受控虚拟 HTTPS 来源的资源提供器，同时提供 SPA 入口与静态资源；验证路径在 UI 根目录内，拦截外部顶层导航。该实现解决本机直接文件夹映射没有接通 SPA 文档的问题。
- 根据最终 HTML 生成 bootstrap 脚本哈希 CSP；生产脚本不使用 unsafe-inline。生产 bundle 已检查不包含开发预览种子内容或 previewInvoke。
- Playwright 在父进程设置并继承测试端口，避免 worker 配置重算不同端口。

## 未覆盖与后续范围

完整 ECDICT 下载/大库导入、新版学习与记忆算法、词汇写入、统计、AI/凭据、真实语音及安装/升级/卸载尚未验收；这些是各开发 Agent 的任务。原版 exe 未在本次启动，尚未做完整原版对照或逐像素验收。

依赖审计已完成非破坏性修复，消除了报告中的高严重度项；剩余 3 个低严重度 SvelteKit/adapter/cookie 相关项。npm 给出的完整修复需要跨到 SvelteKit 3 / adapter 4，本次保留原版兼容的主版本，交由 A 评估升级。生产只分发静态前端，未部署 Node SSR 服务。

基础工程可以支撑并行开发；Phase 0 的可用查询与页面迁移不表示其余 28 个业务命令已经实现。未实现能力始终由诊断目录和错误响应明确标记。

## 平台增量（合同 1.1.0）

日期：2026-10-26。协议仍为 1，schema 仍为 5。合同增加 `lane=background`，不改变 DTO。

| 验证 | 结果 |
|---|---|
| `node tools/generate-contracts.mjs --check` | 通过 |
| `node tools/verify-baseline.mjs` | 10 个固定文件通过 |
| MSVC Release 构建 | 通过 |
| `ctest -C Release` | 6/6 通过，含 parity_platform_test |
| `npm run check` | 0 错误、0 警告 |
| `npm run build` | 通过 |
| `node tools/smoke-rpc.mjs`（Release 服务） | 14 项通过：真实 HTTP、中文数据目录、`/events` 首包与并发查词 |
| WebView2 `--smoke-report` | ok=true，contract_version=1.1.0，ui_rendered=true，中文数据目录 |
| `tools/check-runtime.ps1` | WebView2 Runtime 155.0.4283.62 |
| 安装、再次安装、卸载到临时目录 | 程序目录被替换并删除；个人数据标记仍在 |
| 原版库只读探测 | `%APPDATA%\com.lexica.app\lexica.db`，1,033,101,312 字节，user_version=4，能查到 meticulous。缺的表只有 migration 5 的 `word_enrichments` 和 `custom_wordbook_words`。探测后字节数不变 |

冒烟仍会在命令未注册时确认写入不会假装成功。成功条件是查词链和能力目录，不再把“永远返回 NOT_IMPLEMENTED”当成功能完成。

依赖审计仍是 3 个低严重度项：`@sveltejs/kit`、`@sveltejs/adapter-static`、`cookie`。完整修复要升到 SvelteKit 3 / adapter 4，会离开原版主版本。生产包只带静态页面，没有 Node SSR。本次不升级。

未在本次启动原版 exe，也没有把那份 1GB 的 version 4 库复制后做写入升级。version 4 追加到 5 由小型夹具验证。文件选择对话框和开始菜单快捷方式没有做界面点击验收。
