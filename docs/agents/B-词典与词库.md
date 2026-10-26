你是 LexicaReproduce 的 B，负责词典与词库。原版 SHA 固定为 c4b92a39402860566bf954f92efc8e59666aaa4a；技术路线为 C++ 后端/桌面壳 + 原版 Svelte 界面。

先读 docs/parity/README.md、CONTRACT.md、SOURCE_MAP.md、contracts/models.json、commands.json 和 src/parity/contracts/ports.h。从总控指定的 Phase 0 SHA 创建 codex/parity-dictionary 独立 worktree，运行 tools/sync-reference.ps1 获取固定源码。

可写：src/parity/modules/dictionary/**、tests/parity/dictionary/**，包括模块自己的 CMakeLists.txt。公共合同、共享存储/迁移、资源种子和根构建文件由 A 维护；提出具体变更需求并提供草案。

任务：

1. 实现 DictionaryPort 的完整 find/byId/suggest，替换当前 SeedDictionary 的简化规范化和查询。
2. 对齐原版 dictionary/engine.rs、store.rs、model.rs：NFKC、大小写、引号、变音符、精确/词形回退；建议的精确优先、词频及字母顺序。
3. 按原版 provider.rs 导入 ECDICT 元数据，完整返回有序义项、例句、词形、词频、标签、搭配和扩展内容；对齐 roots.rs 等派生逻辑，缺字段如实处理。
4. 实现本模块四个命令 app_info、search_suggest、import_ecdict、ensure_dictionary；后两者对齐 bootstrap.rs 的下载、解压、导入、状态检查和进度阶段。网络/解压在 DB 锁外，长导入用一致配置的独立连接与短事务。
5. 保持种子/用户数据和 ID 稳定、重复导入幂等、失败不破坏已存在学习数据；旧导入暂存/并发保护可以借鉴，但必须适配冻结的新 schema。

接口边界：DictionaryPort 只读，不能记录 encounters 或修改记忆；lookup_word 命令归 C，C 通过你的端口查询。每个端口接收的 sqlite3* 已处于调用者的连接回调内，不再获取 Database 锁或自行开启事务。

必须测试：NFKC/弯引号/变音符/词形/建议排序；多义项顺序和 nullable；种子 55 条幂等；真实 ECDICT 小样本和大库性能；中断/坏文件/重复导入；导入期间真实查词与收藏/笔记写入。用原版固定输入输出建立黄金用例。

完成后将注册的 seed_only 状态更新为 implemented；只有真实完整词库就绪才返回 dictionary_ready=true。当前 UI 的自动 setup 由 ensure_dictionary 是否真实注册控制，与你的后台实现一起验证，避免空实现触发自动大下载。

交付：功能族 LOOK-01/02、WORD-02/03、DATA-01/02，参考源码位置、提交 SHA、测试与性能证据、已知数据缺项、需要 A 的迁移/依赖需求。按 docs/agents/README.md 的格式提交。
