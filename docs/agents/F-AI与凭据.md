你是 LexicaReproduce 的 F，负责 AI、凭据与缓存。参考 SHA c4b92a39402860566bf954f92efc8e59666aaa4a，后端以 C++ 实现，保持原版可观察语义。

先读 docs/parity/README.md、CONTRACT.md、SOURCE_MAP.md、contracts/models.json/commands.json 和 ports.h。使用总控指定 Phase 0 SHA，codex/parity-ai 独立 worktree；获取固定参考源码。

可写：src/parity/modules/ai/**、tests/parity/ai/** 和本模块 CMakeLists.txt。公共 schema/DTO、根链接依赖、C/E 的业务与页面由对应 Agent 负责。需要 Windows 库或新端口时向 A 提交具体需求。

任务：

1. 对齐 src-tauri/src/ai.rs 与 commands/mod.rs，完成 ai_config_get/save、ai_test_connection、ai_generate_examples、ai_enrich_words。
2. 默认 DeepSeek 地址与模型，默认关闭；OpenAI 兼容 Chat Completions，一次请求 15 秒超时、不自动重试。
3. 实现 Windows 凭据管理器 KeyStore，采用复刻版独立 service/account 标识。key=null 保留、空串删除、非空替换，回读只返回 has_key。
4. 通过 DictionaryPort/冻结 schema 装配单词与义项；验证 sense 所属、整词目标、非空内容和结构，合格后才缓存。按词、义项内容、地址、模型和提示词版本区分缓存。
5. 移植批量例句和扩展内容补充的输入/校验/缓存行为，区别原词典、AI 生成内容和练习挖空文本，不覆盖原始词典数据。
6. 配置/缓存短锁读→锁外网络→短事务写；使用可注入 HttpClient/KeyStore，离线与失败时能让 C/E 继续本地题目。

约束：密钥不进入 SQLite、localStorage、日志或响应；凭据失败直接报告，不明文降级。联网只发送原版约定的词与义项，不发送私人笔记/历史。不要把旧 C++ 环境变量密钥方案标记为已复刻 Windows 凭据能力。

必须测试：配置三态；无 key/关闭 AI/超时/网络失败/非法 JSON；错误 sense、目标子串、大小写/多次目标；缓存命中/失效；并发请求和 DB 锁外等待；扩展内容字段校验；日志和响应无密钥。自动测试用假网络和内存 KeyStore，不消耗用户真实 API 额度。实机凭据测试使用临时独立标识并清理自己的测试项。

交付 AI-01/02、命令实现、提交 SHA、真实存储/假网络测试结果、缓存键说明、已知差异以及需要 A 的依赖/迁移需求，按统一模板提交。
