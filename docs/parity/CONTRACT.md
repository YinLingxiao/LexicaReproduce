# 公共接口与数据合同 v1.0.0

权威输入：`contracts/models.json`、`contracts/commands.json`。C++ 生成文件为 `src/parity/contracts/dto.h`、`catalog.h`；TypeScript 为 `ui/src/lib/generated/contracts.ts`。

```powershell
node tools/generate-contracts.mjs
node tools/generate-contracts.mjs --check
```

生成器不依赖 npm 包。A 审核合同变更后修改 JSON 并重新生成；禁止直接改生成物。原版 API 快照保留在 `contracts/upstream-api.ts`。`tools/migrate-ui.mjs` 是一次性迁移工具，后续不要运行它覆盖人工修订。

## 请求和响应

```json
{"protocol_version":1,"request_id":"lookup-1","command":"lookup_word","args":{"word":"meticulous"}}
```

```json
{"protocol_version":1,"request_id":"lookup-1","ok":true,"result":{"outcome":{"type":"hit","entry":{}},"encounter_id":null,"memory":null,"comprehension_level":null}}
```

上例 entry 为缩写；实际响应满足完整 WordEntry 模型。无返回值的成功结果是 JSON null。失败没有 result：

```json
{"protocol_version":1,"request_id":"save-1","ok":false,"error":{"code":"NOT_IMPLEMENTED","message":"save_word_note is scheduled for the next development phase."}}
```

- 参数使用原版调用的 camelCase，结果字段沿用 snake_case。
- request_id 为最长 128 字节的字符串，用于关联请求。它是传输标识，不自动等同于学习事件或正式题 ID。
- 数字 ID 限制在 JavaScript 安全整数范围内；UTC 时间在存储/DTO 中使用固定 24 字符 RFC3339 毫秒字符串，计算接口使用 UtcMillis。
- 所有字段、nullable、默认参数、结果类型与命令所有者见 commands.json。缺省 nullable 参数被补为 null；limit 的缺省和 null 都按原版默认值处理。
- Router 拒绝未知参数、错误类型、非法枚举及明确的边界越界，并校验返回结果。
- 正式复习的防重复以 review_id 与事务内状态为准；study_grade 的重发语义由 C 明确测试，并通过 Context.request_id 区分同一请求重发与新的主动评分。
- 新模块通过 `router.bind("所有者", "命令", handler, "implemented")` 注册；重复绑定和越权所有者立即报错。注册必须在 Application 构造完成前进行，运行中不修改 Router。

## 模块接口

`DictionaryPort` 提供 find、byId、suggest，输入 sqlite3* 来自当前数据库回调。查询方法保持只读，不获取 Database 锁、不创建事务、不记录接触事件。B 实现端口；C/F 可在模块测试中替换夹具。

`Context` 提供 Database、可注入毫秒 Clock、DictionaryPort、EventSink 和逐请求 request_id。dispatch 会复制 Context；各 Agent 自有缓存或共享状态须自行同步。

AI 的 `KeyStore` 与 `HttpClient` 是可替换边界。真实凭据存储使用复刻版独立命名空间，避免修改原版应用密钥。HTTP 默认保持 15 秒超时，不持有 DB 锁发送网络请求。

## 事务和锁

```cpp
router.bind("library", "word_note", [](const Json& input, Context& context) -> Json {
    const auto args = input.get<WordNoteArgs>();
    return context.db.withConnection([&](sqlite3* db) -> Json {
        // 参数绑定的 SQL；返回 WordNote。
        // 写操作在这里创建 Transaction，并把一个业务动作原子提交。
        throw Error("NOT_IMPLEMENTED", "Example only");
    });
});
```

- Database::withConnection 对单个数据库连接串行化。sqlite3*、Statement 和 Transaction 不得逃逸出回调，不得嵌套获取同一 Database 锁。
- Statement 的绑定、step 和 execute 失败均抛 DB_ERROR；Transaction 作用域退出未 commit 时回滚。
- 长导入按 B 的设计使用独立连接和短写事务；连接配置、迁移版本和锁顺序遵循共享存储规范。提交前需通过并发写入测试。
- 网络、解压、文件扫描和耗时计算在数据库锁/事务外进行；事件回调也应在锁外发送。
- 桌面使用后台工作线程，UI 主线程只负责接收/发送消息与窗口事件。关闭时停止接收并等待在途操作；后续长任务由 A/B/F 协同加入取消机制。

## 数据库

直接采用固定参考版本的五次 SQL 迁移与表语义：

| 层次 | 表 |
|---|---|
| 基础词典 | words、senses、examples、collocations、word_forms、words_fts |
| 事实与行为 | encounters、reviews、review_attempts |
| 记忆投影 | memory_states |
| 个人内容 | word_notes、ignored_words、custom_wordbook_words |
| 可选增强 | app_settings、ai_example_cache、word_enrichments |

encounters 与完成的 reviews 为投影事实源；前端偏好仍放在原版 localStorage 键中，不另存词典数据库。迁移由 A 单独维护连续版本；B/C/D/F 在模块交付中提交所需迁移草案，不运行自有 schema 升级。

新数据库文件独立于旧 C++ 库。无版本且非空的库被拒绝，避免误改旧库；高于当前 user_version 的库被拒绝。旧数据迁移和真实原版数据库兼容验收属于 A 后续任务，Phase 0 只验证版本迁移与保留样本记录。

## 事件与能力

```json
{"protocol_version":1,"event":"dictionary-setup","payload":{"phase":"importing","progress":null,"message":"Importing…"}}
```

事件没有 request_id。原版 dictionary-setup 的阶段及 payload 已进入模型。Context.emit 可为空，调用前检查；HTTP 开发适配器当前只提供 RPC 响应，异步进度的浏览器订阅由 A 后续补齐，桌面 EventSink 已接通。

`system_capabilities` 返回合同、foundation_phase、逐命令 available/implementation 和 automatic_dictionary_setup。implementation 可为 implemented、seed_only、read_only_seed；未注册为 not_implemented。新增功能应通过实际注册而非修改 UI 伪造可用性。

## 错误码

基础错误：BAD_REQUEST、UNKNOWN_COMMAND、PROTOCOL_MISMATCH、NOT_IMPLEMENTED、CONTRACT_ERROR、DB_ERROR、SCHEMA_MISMATCH、VERSION_TOO_NEW、DATA_DIRECTORY_ERROR、NETWORK_ERROR、RESOURCE_ERROR、WEBVIEW_ERROR、BUSY、INTERNAL_ERROR。

前端传输还会产生 TIMEOUT、CANCELLED、HTTP_ERROR。业务未命中是 LookupOutcome.miss，空集合是成功响应。新增业务错误码由 A 记录后统一使用，保留可读 message；响应和日志不得携带密钥。

## 已确认的兼容适配

原版 Rust `practice_check` 实际返回 bool，原前端却声明 `{correct:boolean}`。本合同保留 bool wire 结果，`ui/src/lib/api.ts` 显式适配成页面需要的对象。这项修正应保留，并加入 C/E 的真实接口测试。
