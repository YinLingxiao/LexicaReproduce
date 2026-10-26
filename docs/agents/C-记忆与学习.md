你是 LexicaReproduce 的 C，负责记忆、词书、学习和复习。原版 SHA c4b92a39402860566bf954f92efc8e59666aaa4a；C++ 后端/桌面壳 + 原版 Svelte，目标行为等价。

先读 docs/parity/README.md、CONTRACT.md、SOURCE_MAP.md、contracts/models.json/commands.json 和 ports.h。使用总控指定的 Phase 0 SHA、codex/parity-learning 独立 worktree，获取固定参考源码。

可写：src/parity/modules/learning/**、tests/parity/learning/** 和本模块 CMakeLists.txt。公共 DTO、迁移、共享 Router/存储、前端和根构建由其他所有者维护。你可先用 DictionaryPort 夹具并发开发，不等待 B 完成。

任务：

1. 移植 memory/model.rs、scheduler.rs、service.rs、store.rs，使用可注入毫秒 Clock。encounters 和已完成 reviews 为事实源，memory_states 可重建。
2. 对齐全部调度参数、事件同刻顺序、理解级别更新、浮点和毫秒舍入。初始强度为 2/1/0.5/0.25 天，熟悉/稳定阈值为 4/21 天；弱访问不可推迟 due。
3. 把 lookup_word 从当前 read_only_seed 升级为真实查词事件、理解状态和投影的事务编排，通过 DictionaryPort 取词，禁止沿用旧 Review 的固定 1/3/7/14 天规则。
4. 实现本模块 commands.json 中全部 learning 命令：set_comprehension、rebuild、词书/学习组/评分、自定义词书、review_queue/group、建正式题、提示、提交、practice_check/hint。
5. 对齐 review/service.rs 和 grader.rs：study 20 卡、legacy review 10 条；know/hazy/forgot 映射；词书/UTC 日配额/取词顺序；中文优先本地兜底、首字母/英文/中文提示、长短词容错、全部整词遮空。
6. 正式题和重练的成绩计数严格区分，提交失败可恢复，重复提交不重复记正式成绩。study_grade 区分请求重发和新的主动评分，明确事务和防重策略。

原版边界按 SOURCE_MAP 的 COMP-01～09/11 处理；planStyle 未发现实际算法参数，先保留源码行为并记录动态核验。到期词书过滤和 UTC 日界不自行重设计。

practice_check 的 wire 结果是 bool，公共前端已适配成 {correct}。新学习卡评分与正式填空是不同流程；Previous/Next 仅浏览，页面重排由 E 按原版实现。

必须测试：原版事件序列黄金输出、未来/同刻/跨日时间、弱查词不推迟到期、遗忘/强化/优先级清除、重建一致；各词书/配额/排序；正式题防重/重练不重复记数；提交事务失败；暂停词和缺素材词；与 B/D 实际数据库联调。测试不要真实等待天数。

交付功能族 WORD-01、STUDY-01/02/04、REV-01/02、MEM-01，提交 SHA、验证记录、未解决兼容项和需要 A 的迁移/合同变更。按统一交付模板提交。
