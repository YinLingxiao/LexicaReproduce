# 原版对照验收

日期：2026-10-27。对照基准：`YinLingxiao/Lexica.` 提交 `c4b92a39402860566bf954f92efc8e59666aaa4a`（0.2.0）。本地快照：`reference/lexica`，`git rev-parse HEAD` 与该 SHA 一致。复刻合同仍为 1.1.0，协议 1，schema 5。

原版程序未能启动：本机没有 Rust/Cargo。界面对照依据固定源码和复刻版实际页面。原版数据库只做只读探测，写入只发生在独立副本。

状态用词：已验证一致、已修复并验证、存在明确差异、外部条件不足尚未验收。

## 界面

页面 Svelte、`app.css`、偏好、音节和语音源文件与固定提交一致。差异只在传输层：`dictionary-setup.svelte.ts` 在后端声明不能自动准备词库时跳过下载；复习收藏在笔记读取失败时不再把笔记写成空字符串；设置页外链改走本机 `openUrl`。

| 项 | 状态 | 证据 |
|---|---|---|
| Search / Words / Review / Stats / Settings 的文案、组件和样式源码 | 已验证一致 | `git diff --no-index` 对上述页面和组件，除上面三处适配外无差异 |
| 1180×820 与 480×820 CSS 视口 | 已验证一致 | 浏览器把视口设为这两个尺寸。480 时 `innerWidth=480`，底栏为 grid，侧栏导航隐藏，顶栏保留品牌。截图是工具面板裁切，不是逐像素原版窗口 |
| 深色主题下的查词和设置 | 已验证一致 | 系统深色。查词页有音节、音标、词频、CEFR-C1、例句。设置页显示 3,401,187 条和 ECDICT ready |
| 原版窗口并排逐像素 | 外部条件不足、尚未验收 | 无 Rust，未启动原版 exe。字体抗锯齿和系统深色不记为缺陷 |
| 窗口图标和开始菜单图标 | 已修复并验证到文件布局 | 使用原版 `icon.ico`。桌面窗口从程序目录加载 `lexica.ico`，安装快捷方式指向该图标。未另做任务栏截图 |
| 发音、文件选择、外链点击、剪贴板实际写入 | 外部条件不足、尚未验收 | 源码路径与原版相同。WebView 在 `navigator.clipboard.writeText` 失败时改走已有的宿主写入。本轮没有点击这些控件 |
| 输入法、快捷键和高 DPI 实机 | 外部条件不足、尚未验收 | 快捷键处理在与原版相同的 Svelte 里。窗口创建使用每显示器 DPI，内容尺寸 1180×820，最小 480×400 |

截图：

- [查词 1180](evidence/acceptance-search-1180.png)
- [设置 1180](evidence/acceptance-settings-1180.png)
- [查词 480](evidence/acceptance-search-480.png)

渲染环境：Windows，系统深色，复刻版 HTTP 服务加载已构建 UI，数据为本次下载导入的 ECDICT。动态访问次数不作为差异。

## 数据与命令

| 项 | 状态 | 证据 |
|---|---|---|
| 只读原版库 | 已验证一致 | `%APPDATA%\com.lexica.app\lexica.db` 为 user_version 4，3,401,187 词，其中 ECDICT 3,401,137。探测前后大小都是 1,033,101,312。没有 `word_enrichments` |
| 副本迁移 4→5 | 已修复并验证 | 只读 backup 到 `build-parity-msvc/test-output/acceptance-migrate/lexica.db`。打开后 schema 5，词数不变，笔记 2 条仍在。查词 `meticulous` 命中，并在副本上新增 1 条接触。原版库版本仍是 4 |
| 完整 ECDICT 下载、解压、导入、标记、重启 | 已验证一致 | GitHub 返回 200，包 216,765,132 字节。`ensure_dictionary` 导入 3,401,132，总词数 3,401,187，`ready=true`。`ecdict.ok` 内容为 `1`，zip 已删除，`stardict.db` 保留。进程退出后再次 `app_info` 仍是 schema 5 且 dictionary_ready |
| 导入期间查词 | 存在明确差异 | 78 次 `lookup_word(meticulous)` 中 69 次命中、9 次返回非命中，0 次传输异常。当时 busy timeout 为 2 秒，原版是 5 秒。已改为 5 秒。未再用全量导入复测这 9 次 |
| 完成标记 | 已修复并验证 | 原版 `app_info` / `ensure` 在 `ecdict.ok` 存在时视为已准备。复刻版此前只看词数。现为标记或词数达到 1,000,000。词典测试覆盖“只有标记、词数仍是种子”时不再下载 |
| 查词链、学习、词库、AI 算法 | 已验证一致 | 规范化、建议排序、调度参数、学习 20 张、复习 10 条、提示顺序、UTC 日界和活动图与固定 Rust 源码对应。本轮没有改这些阈值 |
| 付费 AI 供应商 | 外部条件不足、尚未验收 | 没有用户提供的测试密钥。不把密钥写入代码或日志 |
| 发音听感、快捷方式点击、新机器安装 | 外部条件不足、尚未验收 | 快捷方式图标路径已接上。未在无开发环境的机器上重装 |

导入和重启日志在 `build-parity-msvc/test-output/acceptance-ecdict/run.log`。该目录和数据库副本不进入 Git。

## 本轮代码

- `ecdict.ok` 或 ECDICT 词数达到门槛时，`app_info.dictionary_ready` 为真，`ensure_dictionary` 跳过下载。
- SQLite busy timeout 从 2 秒改为 5 秒，与原版 `database/mod.rs` 一致。
- 窗口和安装快捷方式使用原版图标。
- 桌面里剪贴板 API 失败时使用已有的宿主写入。

未把词频来源、等级来源做成 schema 6。那会离开固定原版的表和界面。
