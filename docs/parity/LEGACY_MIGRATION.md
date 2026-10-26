# 旧 C++ 数据库迁移

复刻版不打开、不升级、不覆盖旧程序的 `lexica.db`。无 `user_version` 且已有业务表的文件会被 `Database` 拒绝，错误码 `SCHEMA_MISMATCH`。

## 识别

| 来源 | 判定 | 处理 |
|---|---|---|
| 原版 Lexica | `user_version` 为 1–5，且有 `words`、没有 `dictionary_entries` | 只读探测；复制后再由正式库打开。版本低于 5 时只追加迁移 |
| 旧 C++ 程序 | 有 `schema_migrations` 与 `dictionary_entries` | `migrateLegacy` 只读源库，把能对应上的个人记录写入新的 parity 库 |
| 其他文件 | 两者都不是 | 拒绝 |

`db_dir.txt` 如果指向 `%APPDATA%\com.lexica.app` 或 `%LOCALAPPDATA%\com.lexica.app`，解析直接失败，避免把原版库当成复刻版库迁移。

## 旧 C++ 记录怎么搬

只搬目标词库里已经存在的词。词条正文仍以种子词或以后的 ECDICT 为准，不把旧 `dictionary_entries` 写进 `words`。

| 旧表 | 新表 | 规则 |
|---|---|---|
| `vocab_item.favorite` / `note` | `word_notes` | 目标已有该词且还没有笔记时插入；不覆盖已有笔记 |
| `vocab_item.paused=1` | `ignored_words` | 同上，按词对齐 |
| `review_state` | 不写入 | 旧的 stage/due 不是原版记忆模型 |
| `vocab_history` / `vocab_lookup_op` | 不写入 | 不制造 encounters、reviews 或 memory_states |

源文件以只读方式打开。迁移报告包含行数、写入数、因目标没有该词而跳过的数量，以及 `source_modified=false`。

调用入口是 `lexica::parity::inspectLegacy` 与 `migrateLegacy`。平台测试会造一个旧库，确认笔记和暂停被搬过去，复习阶段和历史没有变成学习事件。
