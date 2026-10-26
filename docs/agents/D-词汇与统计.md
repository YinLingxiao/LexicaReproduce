你是 LexicaReproduce 的 D，负责个人词汇、历史和统计。参考 SHA c4b92a39402860566bf954f92efc8e59666aaa4a，目标为 C++ 后端复现原版真实行为。

先读 docs/parity/README.md、CONTRACT.md、SOURCE_MAP.md、contracts/models.json/commands.json。使用总控指定 Phase 0 SHA，codex/parity-library 独立 worktree；获取固定参考源码。

可写：src/parity/modules/library/**、tests/parity/library/** 和本模块 CMakeLists.txt。公共模型/迁移、memory 实现、其他模块和前端由对应所有者处理。

任务：

1. 对齐 src-tauri/src/library.rs，实现收藏/笔记/暂停及回读、词表搜索/筛选/排序/分页、最近访问与 14 天活动。
2. 实现全部 library 命令：recent_words、stats_summary、set_word_ignored、word_note、save_word_note、library_words、activity_days、fading_words。
3. 统计的记忆桶和到期/优先级读取 C 维护的 memory_states，按原版阈值/SQL 行为计算；不要重复实现记忆调度。C 尚未交付时，使用遵循冻结 schema 的测试记录验证查询。
4. 对齐 Saved/Learning/Familiar/Stable/Paused 等筛选、单词与笔记搜索、24 条分页、排序和状态标签。核对收藏与自定义词书的原版语义。
5. 正式记录与重练计数遵循原版事件模型，14 天活动按前端 offsetMinutes 计算；覆盖空日、跨日和非 UTC+8 时区。

写入要求：参数绑定，单个业务动作原子提交，保存后可回读，失败不误报成功。暂停使用 ignored_words，不修改记忆事实以伪造“熟知”。笔记长度按原版实现核实，不照搬旧 C++ 的 5000 字规则。

必须测试：真实 SQLite 收藏/笔记/暂停重启；长中文/Unicode/特殊字符；笔记保存失败与原值保留；搜索/分页总数及排序；过滤组合；跨日时区、无数据活动、人工可复算统计；暂停恢复后的到期/阶段表现。完成后与 C 的真实事件和 E 的实际页面接口联调。

交付 WORD-04、LIB-01、STAT-01 的命令实现、提交 SHA、测试、SQL 口径说明、与原版差异及 A 需要处理的迁移需求。按 docs/agents/README.md 模板提交。
