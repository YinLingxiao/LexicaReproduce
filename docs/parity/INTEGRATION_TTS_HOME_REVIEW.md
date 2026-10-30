# TTS 与学习首页集成审核

后续现场启动问题与整页布局修订见 [STARTUP_LOOKUP_FIX.md](STARTUP_LOOKUP_FIX.md)。

日期：2026-10-30。共同基线：`38e56d97b339be213e92f7df30943a31faef50df`。

审核输入：`codex/tts-improvement`（`f3f35a5`）与 `codex/learning-home-improvement`（`0841e04`）。两个分支先以 merge commit 合入 `codex/review-tts-home`，再提交下述修复。Agent 的原交付报告描述的是修复前的分支状态，以本报告和当前源码为准。

## 审核发现与修复

| 问题 | 影响与修复 |
|---|---|
| TTS 取消 token 全进程共用 | 停止发音后刷新页面，新文档从 1 开始的 token 可能全部被拒绝；不同标签页也会互相取消。新增可选 clientId，前端每个文档生成独立标识；后端按会话记录取消序号，限制缓存数量并淘汰闲置会话。 |
| TTS 与词典初始化共用后台线程 | Agent 的桌面验收因此未完成。新增独立、有界 speech 队列，音色查询和合成都与导入分离，取消仍走交互线程。回归测试在模拟长时间导入期间验证合成可执行。 |
| SAPI 等待路径没有检查失败 HRESULT 和自身截止时间 | 补充失败处理，并让 SAPI 与 WinRT 共用取消和执行期限检查。 |
| 音色说明挤压例句 | 实际截图中播放后的例句几乎逐词换行。首屏和完整释义的例句改为文本在上、朗读控件在下，新增播放前后宽度/高度断言。 |
| “学习新词”仍接收混合队列 | study_group 同时返回到期卡片。该入口强制 new_first，并只展示 is_new 卡片；到期入口继续只调用 review_group。桌面侧栏和窄屏导航的 Review 都指向 mode=due，保留原无参数深链行为。 |
| 上一轮异步请求可以修改新一轮状态 | 为加载、评分、练习、提示、AI 等结果增加会话校验，切换模式及离开页面时使旧结果失效；不撤销已经提交到后台的旧词反馈。 |
| 查词提示的学习状态停留在初次响应 | 从已有只读词库接口刷新学习状态，并使用命中词条的标准词形查询；不额外调用 lookup_word，不重建 WordView，不丢失未保存笔记。 |
| 播放边界与资源清理 | 捕获同步播放器异常，按 UTF-8 字节检查文本长度，释放失败播放的 Blob URL，并在学习卡片离开时停止其自动朗读。 |
| 预览用提示音代替发音 | 删除开发预览中的 440 Hz 合成提示音和模拟音色。没有真实后端的预览明确说明发音不可用。只有测试夹具模拟媒体播放，真实验收不替换音频引擎。 |
| 测试仍依赖旧浏览器语音接口 | 更新公共夹具为 TTS RPC 契约，并将断言改为检查合成文本和口音参数。实时测试改用独立、可重复运行的临时数据目录。 |

合同为 **1.3.0，34 个命令**；协议仍为 1，数据库 schema 仍为 6，本次没有数据库迁移。生成文件通过 `tools/generate-contracts.mjs` 更新。

桌面增加 `--no-auto-setup`，用于隔离测试及诊断；正常启动仍自动准备词典。原生测试通过真实 WebView2 bridge 调用 C++，使用真实 HTMLAudioElement 的 playing 事件证明播放已经开始。

## 验证

| 检查 | 结果 |
|---|---|
| MSVC Debug 构建与 CTest | 11/11 |
| MSVC Release 构建与 CTest | 11/11 |
| UCRT 构建与 CTest | 11/11；TTS 明确返回不支持，其他模块正常 |
| Svelte 检查、生产构建 | 0 错误、0 警告；构建通过 |
| TTS 控制器回归 | 7/7 |
| 原有前端夹具 | 24/24 |
| 首页与复习新增夹具 | 10/10 |
| 真实 HTTP 首页/复习/TTS 流程 | 2/2 |
| 原有真实服务 UI 回归 | 3/3 |
| 打包后的 Release HTTP 与 UI | 18 项 HTTP、2 项新流程、3 项原流程 |
| 打包后的原生 WebView2 | 设置试听、单词、例句、刷新后发音及例句布局 |
| 安装、升级、卸载 | 中文路径回归通过，保留非安装器拥有的文件和个人数据 |
| 原版引用基线 | 10 个固定文件校验通过 |

所有新测试都使用构建目录中的隔离数据库。没有连接或修改真实用户学习库，没有启动替代用户日常服务的常驻进程。程序包已更新到 `dist/LexicaReproduce`。

主要日志位于 `build-parity-msvc/review-*.log`。摘要和关键截图保存在 [evidence/tts-home-review](evidence/tts-home-review/)。

## 复现

```powershell
# 主仓库中
cmake --build --preset parity-msvc
ctest --preset parity-msvc
node --experimental-strip-types ui/tests/tts-controller.test.mjs
node tools/test-tts-desktop.mjs dist/LexicaReproduce/lexica_desktop.exe dist/LexicaReproduce/ui
node tools/smoke-rpc.mjs dist/LexicaReproduce/lexica_parity_server.exe

# ui 目录中
npm run check
npx playwright test -c playwright.fixture.config.ts
npx playwright test -c playwright.learning-home.config.ts
$env:LEXICA_SERVER_EXE = (Resolve-Path ../dist/LexicaReproduce/lexica_parity_server.exe).Path
$env:LEXICA_UI_DIR = (Resolve-Path ../dist/LexicaReproduce/ui).Path
npx playwright test -c playwright.learning-home-live.config.ts
npx playwright test
```

## 验证边界

当前机器可用音色为 Microsoft Zira Desktop（美式英语）。英音请求会明确提示使用了实际的美式音色，不冒充英式发音。本机无法完成“真实英音音色”的试听验证。

C++ 测试检查真实合成 WAV 存在非零 PCM；浏览器和原生测试检查真实媒体 playing 事件。**这些不等于人工听音或发音质量评测**。仍需用户实际试听确认音量、音色和发音质量。

原始音频上限为 768 KiB，文本上限为 2000 个 UTF-8 字节，过长内容会明确失败。没有接入云 TTS。首页的价值主张是产品目标，没有声称已证明记忆效果提升。
