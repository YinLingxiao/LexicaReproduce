# 单词与句子 TTS

分支 `codex/tts-improvement`。worktree：`D:\lexica-tts-improvement`。起始 SHA `38e56d97b339be213e92f7df30943a31faef50df`。实现提交 `72d9c6de3ed13bbc28fd33e865c378646a428e44`。未合并、未推送。

## 方案

桌面版和 HTTP 网页版走同一套 RPC。不接云 TTS，不要求 API Key。

音频放在 JSON 的 `audio_base64` 里，前端自己建 Blob URL。现有 `POST /rpc` 请求体上限是 1 MiB，合成请求只是短文本；响应不走这个上限。单词和例句的 WAV 限制在 768 KiB 原始字节以内（base64 后大约 1 MiB），WebView2 和 HTTP 都能装下。不返回本地路径，也不新开读文件接口。文本最长 2000 字节。合成在 background 队列，取消在交互队列，这样查词和复习不会排在朗读后面。后台队列本身最多 8 个任务。前端一次只保留最新的一条合成，旧结果用 token 丢弃。

Windows 语音有两层：

- `Windows.Media.SpeechSynthesis` 列出 OneCore 英语音色。MSVC 且 Windows SDK 自带 C++/WinRT 头文件时编译进来，并链接 `runtimeobject`。
- 桌面 SAPI 音色（本机的 Microsoft Zira Desktop）不在 OneCore 列表里，所以再枚举 `SPCAT_VOICES` 里的英语 token，用 `ISpVoice` 合成到内存流。语速 0.8 / 1 / 1.2 分别映射为 SAPI rate -2 / 0 / +2。OneCore 音色则把 0.8、1、1.2 直接交给 `SpeakingRate`。

没有 WinRT 头文件的构建（包括 UCRT）仍编译，`tts_voices.supported` 为 false，合成返回 `NOT_SUPPORTED`。本机用 g++ 16.2（UCRT，PATH 含 `C:\msys64\ucrt64\bin`）对 `engine.cpp` 和 `module.cpp` 做了 `-DLEXICA_TTS_WINRT=0 -fsyntax-only`，通过。没有跑完整 `parity-ucrt` 预设。

WinRT 调用放在单独线程上，该线程 `winrt::init_apartment(multi_threaded)`。取消会 `IAsyncOperation::Cancel()`；SAPI 用 `SPF_ASYNC` 轮询，再 `SPF_PURGEBEFORESPEAK`。合成超过 30 秒记为失败。

## 契约 1.3.0

增量命令，旧命令参数和结果不变。协议仍是 1。

| 命令 | 队列 | 作用 |
|---|---|---|
| `tts_voices` | 交互 | 英语音色的 id、名称、语言、口音（`US` / `UK` / `OTHER`）。`supported=false` 表示构建没有系统语音。 |
| `tts_synthesize` | background | `text`、`token`、可选 `voiceId` / `accent` / `rate`。返回 mime、base64、实际音色和 `substituted`。 |
| `tts_cancel` | 交互 | 提高 token。只打断更早的合成，不把同 token 的新合成取消掉。 |

`rate` 允许 0.5 到 2。界面只用 0.8、1、1.2。指定了 `voiceId` 但找不到时返回 `RESOURCE_ERROR`，不偷偷换音色。没指定音色且请求的英音或美音没有时，改用其他英语音色，`substituted=true`，`actual_accent` 是实际口音。没有英语音色时 `RESOURCE_ERROR`，message 写安装步骤。错误码新增 `NOT_SUPPORTED`。

安装路径按微软当前说明核对：设置 → 时间和语言 → 语言和区域 → 添加语言，安装「英语(美国)」或「英语(英国)」时勾选「文字转语音 / Text-to-speech」。语言已在列表里时，点「…」→ 语言选项，下载「文字转语音」。已安装的语音在设置 → 时间和语言 → 语音里查看。不要求把 Windows 显示语言改成英语。

## 前端

`speech-controller.ts` 管 idle / loading / playing / error、停止、新请求取消旧请求、晚到结果不抢播、重复间隔 350 ms 及定时器清理、Blob URL 和 Audio 释放。正常停止和 `CANCELLED` 不当成播放失败。`play()` 被拒绝且是 `NotAllowedError` 时显示“自动播放被拦截，请点击按钮手动朗读。”，不用静音音频冒充成功。

`SpeakButton` 用在单词标题、首屏例句、完整释义例句和学习卡。按钮文字是「朗读单词」「朗读例句」「正在合成」「停止」。单词读 `entry.word`，不读带音节点的 `display`。例句读英文 `text`。HTTP 页面实际发出的查词朗读请求是 `{"text":"meticulous",...}`。

设置页可以选系统英语音色、试听、改语速。口音、重复次数和自动播放仍用原来的学习设置。没有对应口音时，设置页和播放结果都会写明实际音色。偏好音色 id 存在 `lexica.tts.voiceId`，不改学习偏好的数据结构。

CSP 增加 `media-src 'self' blob:`（`ui/svelte.config.js` 和 `tools/finalize-ui.mjs`）。否则生产页的 Blob 音频会被 `default-src 'self'` 拦住，按钮会显示播放失败。

## 修改文件

- 契约与生成物：`contracts/commands.json`、`contracts/models.json`、`src/parity/contracts/catalog.h`、`src/parity/contracts/dto.h`、`ui/src/lib/generated/contracts.ts`、`docs/parity/CONTRACT.md`
- C++：`src/parity/modules/tts/`、`src/parity/modules/modules.h`、`src/parity/app/application.cpp`、`src/parity/CMakeLists.txt`
- 前端：`ui/src/lib/tts.ts`、`ui/src/lib/speech-controller.ts`、`ui/src/lib/components/SpeakButton.svelte`、`WordView.svelte`、`StudyWordCard.svelte`、`ui/src/routes/settings/+page.svelte`、`ui/src/lib/api.ts`、`ui/src/lib/preview.ts`、`ui/svelte.config.js`、`tools/finalize-ui.mjs`
- 测试计数：`tests/parity/foundation_test.cpp`、`tests/parity/learning/learning_test.cpp`、`tests/parity/platform_test.cpp`、`tools/smoke-rpc.mjs`（可用命令数改为合同长度）
- 新测试：`tests/parity/tts/tts_test.cpp`、`ui/tests/tts-controller.test.mjs`

没有改 `ui/src/routes/+page.svelte`、`ui/src/routes/review/+page.svelte`，也没有改首页学习进度组件。这两个页面如果继续用 `WordView` 和 `StudyWordCard`，不需要再改。不要在页面里直接调用 `speechSynthesis`。

## 构建和测试

在 worktree 中：

- `npm run check`：0 error。
- `npm run build`：通过。CSP 修改后又构建了一次。
- `cmake --preset parity-msvc` 后构建 `parity_tts_test`、`lexica_parity_server`、`lexica_desktop`，以及一次完整 `cmake --build --preset parity-msvc`。
- `ctest --preset parity-msvc`：11/11 通过，含 `parity_tts_test`、`parity_foundation_test`、`parity_platform_test`、`parity_learning_test`。
- `node --experimental-strip-types ui/tests/tts-controller.test.mjs`：5 项通过。覆盖无英语音色、合成失败、播放失败、自动播放拦截、连续点击后旧结果不播放、停止后不重复、语速/音色/重复次数。

`parity_tts_test` 用假引擎覆盖无音色、合成失败、过期 token、飞行中的取消、语速和音色。然后换回系统引擎。

## 真实合成、播放和试听

本机 OneCore 只有中文语音。SAPI 有 Microsoft Zira Desktop（en-US）。请求美音读 `hello`：`substituted=false`，55142 字节 WAV。请求英音读 `The early bird catches the worm.`：实际仍是 Zira，`substituted=true`，`actual_accent=US`，136050 字节。两段都有非零 PCM。

`System.Media.SoundPlayer.PlaySync()` 播放了这两段 WAV，调用返回且没有异常。我没有亲耳听到声音，不能把这一步写成听音验收。

HTTP：临时数据目录、独立端口、`--no-auto-setup`，没有动用户数据库，也没有停用户正在跑的服务。`tts_voices` / `tts_synthesize` 返回上述载荷。Headless Edge 打开 `/settings`，试听按钮变成「停止」，控制台没有 CSP 报错。这证明 `HTMLAudioElement.play()` 进入了播放状态，不是我听到了声音。同一页把英音缺失写在设置里，并在朗读结果里写成“请求的英式英语不可用，实际使用 Microsoft Zira Desktop（美式英语）”。查词页对 meticulous 发出的合成文本是 `meticulous`，旁边有「朗读例句」。

桌面：`lexica_desktop.exe --data-dir <临时目录> --ui-dir ui/build`，用 WebView2 远程调试连上 `https://lexica.local/settings`。面板列出 Zira，并说明没有英式英语。点击试听后按钮进入「正在合成」。词典初始化对话框占着 background 队列，20 秒内合成没有结束，所以没有把桌面播放推进到「停止」。桌面进程随后被关掉，临时目录在 `build-parity-msvc/test-output/tts-desktop`。

## 限制

- 词典下载/导入也在 background 队列。导入进行时朗读会等，但不会堵住查词和复习。
- 只有 SAPI 音色时，0.8 / 1 / 1.2 是三档 rate，不是 WinRT 的连续 `SpeakingRate`。
- 原始音频超过 768 KiB 会失败。
- UCRT 构建明确不支持系统语音，没有做完整 UCRT 链接和 CTest。
- 没有做人工听音。

## 审核者复现

```powershell
cd D:\lexica-tts-improvement
node --experimental-strip-types ui/tests/tts-controller.test.mjs
cmake --build --preset parity-msvc --target parity_tts_test lexica_parity_server lexica_desktop
ctest --preset parity-msvc -R parity_tts_test --output-on-failure
npm --prefix ui run check
npm --prefix ui run build
.\build-parity-msvc\src\parity\Debug\lexica_parity_server.exe --port 8777 --no-auto-setup --data-dir $env:TEMP\lexica-tts-review --ui-dir .\ui\build
```

浏览器打开 `http://127.0.0.1:8777/settings` 试听，再打开 `http://127.0.0.1:8777/?word=meticulous` 朗读单词和例句。桌面用另一个临时 `--data-dir`，等词典初始化结束后再点试听。没有英语语音包时，设置页应出现上面的 Windows 安装路径，而不是静默失败。
