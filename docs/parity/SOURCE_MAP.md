# 固定源码映射与兼容决定

参考仓库 `YinLingxiao/Lexica.`，提交 `c4b92a39402860566bf954f92efc8e59666aaa4a`。路径均相对此参考仓库。运行 `tools/sync-reference.ps1` 可在当前 worktree 的忽略目录 `reference/lexica` 获取快照；每个 Agent 自行验证 SHA。

## 功能追溯与测试入口

| 功能族 | 参考源码 | 本地归属 | 必须建立的行为用例 |
|---|---|---|---|
| LOOK-01/02 | dictionary/engine.rs、store.rs；SearchBox.svelte | B + E | 规范化、变音符、词形、精确优先/词频建议；输入法/竞态 |
| LOOK-03/WORD-01 | commands/mod.rs、domain/comprehension.rs、memory/service.rs；routes/+page.svelte、WordView.svelte | C + D + E | 一次查词事件；理解级别升级；返回与深链接 |
| WORD-02/03 | dictionary/model.rs、provider.rs、store.rs、roots.rs；WordView.svelte | B + E | 义项/例句顺序、缺字段、标签、关联词和扩展内容 |
| WORD-04/LIB-01 | library.rs；library/+page.svelte、WordView.svelte | D + E | 收藏/笔记/暂停重启；分页/排序/搜索/失败保留草稿 |
| STUDY-01/02 | review/service.rs::study_group、wordbook_filter、wordbooks_list | C | 词书标签/回退、日配额、顺序、新词与到期词 |
| STUDY-03/04 | review/service.rs::study_grade；review/+page.svelte、StudyWordCard.svelte、StudySettingsPanel.svelte | C + E | 评分落盘、forgot 回组末、翻页只读、设置恢复 |
| REV-01/02 | review/grader.rs、service.rs、model.rs；review/+page.svelte | C + E | 逐级提示、长短词容错、多处遮词、正式/重练计数 |
| MEM-01 | memory/model.rs、scheduler.rs、service.rs、store.rs | C | 固定时钟事件序列、浮点/毫秒结果、弱访问、重建 |
| STAT-01 | memory/service.rs、library.rs；stats/+page.svelte | D | 三种掌握状态、到期/优先级、14 天空日/跨日/时区、淡忘词 |
| PREF-01/02 | app.css、routes/+layout.svelte、preferences.svelte.ts、syllables.ts、tts.ts | E | 三风格/深浅/系统、大字、折叠、断点、动效和语音 |
| PREF-03 | preferences.svelte.ts、StudySettingsPanel.svelte | E | 每个默认值、非法存档回退、扩展栏顺序与持久化 |
| AI-01/02 | ai.rs、commands/mod.rs；AiExampleLoading.svelte、api.ts | F + E | Key 三态、义项匹配、目标整词、缓存键/版本、15 秒超时 |
| DATA-01/02 | database/*、dictionary/bootstrap.rs、provider.rs、app.rs | A + B | 迁移、幂等导入、初始化阶段、恢复/跳过、数据目录 |
| DESK-01/02 | tauri.conf.json、capabilities/default.json、app.rs | A | 窗口/DPI/资源/外链、后台关闭、打包与数据保留 |

表中 dictionary/review/memory 路径前缀为 `src-tauri/src/`；Svelte 路径前缀为 `src/lib/components/` 或 `src/routes/`。原版页面测试 `tests/app.spec.ts` 作为案例来源，Rust 各模块内的测试作为后端黄金用例来源。

## 命令所有权

权威列表为 `contracts/commands.json`，共 31 项，名称/参数/结果与 Router 共用生成目录。所有者划分：

- B dictionary：app_info、ensure_dictionary、import_ecdict、search_suggest。
- C learning：lookup_word、set_comprehension、rebuild_memory_projection、study_group、study_grade、wordbooks_list、custom_wordbook_toggle、review_queue、review_group、practice_check、practice_hint、start_review_item、request_hint、submit_review。
- D library：recent_words、stats_summary、set_word_ignored、word_note、save_word_note、library_words、activity_days、fading_words。
- F ai：ai_config_get、ai_config_save、ai_test_connection、ai_generate_examples、ai_enrich_words。

跨模块时由命令所有者编排，避免重复注册同一命令。例如 lookup_word 归 C，查询本身通过 B 的 DictionaryPort，事件/投影在同一个事务中完成。

## 兼容决定和待验证项

| 编号 | 固定源码的实际行为或差异 | 当前决定 | 核验方 |
|---|---|---|---|
| COMP-01 | study_group 最多 20 卡；review_group 最多 10 条 | 分开实现和测试，淘汰旧 C++ 的 12 题规则 | C |
| COMP-02 | know/hazy/forgot → Good/Hard/Forgotten | 严格保留，翻页动作不生成评分 | C/E |
| COMP-03 | 填空提示为首字母→英文→中文；答对质量按提示数递减 | 严格保留，第三级不直接给目标词 | C |
| COMP-04 | 长度至少 5 字符允许编辑距离 1；短词精确 | 严格保留，Unicode 字符而非 UTF-8 字节 | C |
| COMP-05 | 弱 visit 不推迟 due；状态从事件重折 | 原算法参数与舍入逐项移植 | C |
| COMP-06 | planStyle 设置已保存，但 studyGroup 调用没有该参数 | 保留选项与持久化，先验证实际行为，禁止自行发明算法 | C/E |
| COMP-07 | 学习日配额按 UTC，活动图按前端时区偏移 | 作为兼容基线，单独测试；更改需 A 记录新合同 | C/D |
| COMP-08 | 到期学习词未明显按所选词书过滤；缺标签词书存在回退 | 原行为保留并建立针对用例，界面说明不扩展成数据承诺 | C |
| COMP-09 | 学习卡垃圾桶最终置 ignored | 对齐实际暂停行为，不创建“已熟知”虚假事件 | C/D/E |
| COMP-10 | AI Key 的 null/空串/非空分别是保留/删除/替换 | 严格三态，不存明文、不修改原应用凭据 | F |
| COMP-11 | practice_check 后端 bool 与前端对象声明不一致 | wire 保留 bool；API 适配为 {correct}，已在基座修正 | C/E |
| COMP-12 | README 与截图称 50 个种子词，固定 seed.json 实为 55 条 | 以源码为准，Phase 0 导入 55 条并测试幂等性 | A/B |
| COMP-13 | 种子与预览夹具比完整 ECDICT 内容丰富 | 区分数据来源，禁止为全部词编造 CEFR/例句/词根 | B/E/F |
| COMP-14 | Phase 0 lookup 仅查 SQLite，不写学习事实 | 显式 read_only_seed，C 完成事件链后更新注册状态 | C |
| COMP-15 | 旧 C++ 数据结构/复习规则不同 | 隔离新库；旧数据迁移另做报告，禁止覆盖旧用户库 | A |
| COMP-16 | 原版外部平台依赖是 Tauri/Rust | 改为 C++ WebView2/JSON 桥，保持页面业务 DTO 语义 | A/E |
| COMP-17 | `db_dir.txt` 在 app data 中，内容为数据库目录；空则用默认目录 | 复刻版指针放在 `LexicaReproduce/parity-v1`。只接受绝对路径。相对路径拒绝 | A |
| COMP-18 | 原版凭据服务名 `com.lexica.app`，账号 `ai_api_key` | 服务名改为 `LexicaReproduce.parity-v1`，账号不变。不读写原版凭据 | A/F |
| COMP-19 | Tauri WebView 数据在原版 app data，不随 db_dir 移动 | profile 固定在复刻版身份根的 `webview-profile`，不进入原版 `EBWebView` | A |
| COMP-20 | 原版用独立 SQLite 连接做导入，主连接继续查词 | 后台命令单独队列；导入应使用 `withSideConnection` 和短事务 | A/B |
| COMP-21 | 旧 C++ 的 stage/due 与原版记忆事件不是同一模型 | 迁移只搬笔记、收藏和暂停，不生成 encounters 或 memory_states | A |
| COMP-22 | 原版安装器是 Tauri bundle | 复刻版是当前用户目录拷贝。卸载保留 `parity-v1`，除非明确删除数据 | A |
| COMP-23 | 页面用 `navigator.clipboard` 和路径文本框；外链走 opener 插件，允许 GitHub | WebView 允许可信来源读剪贴板，并提供文件/目录选择。http(s) 外链交给系统浏览器 | A |

COMP-06/07/08 的结论来自源码路径检查，尚需原版实机/固定数据动态核对。它们是显式登记的验证任务，不是已证明的产品设计意图。

## 回归样本与隔离

- `resources/seed.json` 为原版固定提交的未改动种子来源，非模型生成的新词典内容。
- `src/parity/storage/migrations/0001`～`0005` 保留原 SQL；新增迁移只追加，不重写旧版本。
- 所有测试输出写到构建目录 `test-output/<模块>/<进程或唯一编号>`；不使用用户数据目录。
- 前端可视测试同时建立“原版确定性夹具”和“真实 C++/SQLite”两层证据。仅通过预览桩测试不等于真实业务完成。
