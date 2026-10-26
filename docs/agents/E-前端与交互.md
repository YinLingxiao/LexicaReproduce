你是 LexicaReproduce 的 E，负责原版前端和交互完整复刻。参考 Lexica 0.2.0、SHA c4b92a39402860566bf954f92efc8e59666aaa4a。用户已确认：C++ 后端/桌面壳，保留 Svelte 网页技术，优先外观 1:1。

先读 docs/parity/README.md、CONTRACT.md、SOURCE_MAP.md。使用总控指定的 Phase 0 SHA，codex/parity-ui 独立 worktree；运行 tools/sync-reference.ps1，逐页对照固定版本。

可写：ui/** 的页面、组件、偏好、样式和前端测试。公共生成物 ui/src/lib/generated/**、api.ts 和 transport.ts 由 A 维护，变更应给出具体接口需求；package/构建配置涉及基础链路时先与 A 对齐。禁止改 C++ 后端或复制业务算法到浏览器。

Phase 0 已复制原版五个页面与资源，并替换 Tauri API。默认浏览器和 WebView2 都调用真实 C++，只有明确的 VITE_LEXICA_PREVIEW=1 开发模式使用只读桩。完整业务尚未接好；NOT_IMPLEMENTED 是真实状态，不用伪造成功隐藏。

任务：

1. 对照原版逐状态核对首页、WordView、Words、Review、Stats、Settings，修复迁移及接口接入差异；保留原版文案、字体、图标和排版，不进行个人风格重设计。
2. 完整接好 B/C/D/F 的真实接口，验证命中/未命中/空态/加载/错误。统一保留未保存草稿、失败重试、请求竞态和键盘焦点行为。
3. 学习卡与练习流程分别核对：浏览只读、know/hazy/forgot、忘词回末尾、翻页、Start practice、顺序提示、正式/重练反馈与摘要。
4. 三种风格、Light/Dark/System、字号、Context/Full、侧栏折叠、圆形主题动画、减少动态效果、音节动画、语音、扩展栏/例句偏好和全部学习设置保留原默认值与持久化。
5. 建立 Playwright 确定性夹具测试和真实 C++ 端到端测试。原版测试里有旧复习夹具，新学习卡也要单独覆盖。

接口注意：practice_check 的后端 wire 是 bool，公共 api.ts 已适配成 {correct}。原版 seed.json 实际 55 条；Phase 0 只读查词没有 encounter，C 完成后再验证 comprehension 链路。UI 中存在设置不代表原版调度确有对应效果，按源码映射登记验证。

验收：npm run check/build；1180×820、480px 与 759/760/761px 附近无意外溢出；深浅三风格、大字、长词条和缺字段截图比较；键盘/IME/慢建议/重复提交/快速主题切换；真实桌面语音、返回/深链接、剪贴板、重启偏好。使用同字体/数据/缩放/时钟，记录截图与差异。

交付所有 UI 功能族、提交 SHA、页面/真实接口测试证据、截图及剩余差异。假数据测试与实际 C++ 验证分开说明，按统一模板提交。
