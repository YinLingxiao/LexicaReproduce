# LexicaReproduce 0.1.0

Windows x64 首个版本，使用 C++17、WebView2 与 Svelte。

## 下载和运行

下载 Release 附件 `LexicaReproduce-0.1.0-windows-x64.zip`，完整解压后，双击 `LexicaReproduce/lexica_desktop.exe`。EXE 需要同目录的 `ui` 等资源，请保留完整文件夹。

需要 Windows 10/11 x64 和 Microsoft Edge WebView2 Runtime。可运行包内 `check-runtime.ps1` 检查运行环境；缺失时安装 [Microsoft WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)。不需要安装 Node、Rust 或 Visual C++ 动态运行库。

首次启动包含 55 条种子词，完整 ECDICT 将自动准备，也可从设置中导入。发布包不包含个人数据库、凭据或完整词库下载缓存。

可选安装：在解压后的程序文件夹打开 PowerShell，执行 `./install.ps1 -Source $PWD`；直接运行 EXE 无需安装脚本。卸载脚本默认保留个人数据。

## 本版功能

- 查词、搜索建议、释义与例句展示。
- 收藏、笔记、自定义词本、暂停和恢复。
- 学习、复习、提示、判分与活动统计。
- ECDICT 导入及可选 AI 配置。
- 对缺失词频、估算等级和短语音节的正确标注。

已验证完整词库导入期间 1,010 次查词零失败，以及数据库版本 5→6 的副本迁移。详细范围见 [验收报告](ACCEPTANCE.md)。原版逐像素对照、真实发音听感、付费 AI 和全新 Windows 机器仍待验收。

程序版本 0.1.0；接口合同 1.2.0；数据库版本 6。这三个版本号分别管理。

Release 同时提供 `SHA256SUMS.txt`，可使用 PowerShell `Get-FileHash <压缩包路径> -Algorithm SHA256` 核对下载文件。
