# D · 个人词汇与统计

负责目录：`d/vocab/`。公开接口见 `vocab.h`，按《分派》合同**没有改动任何公开函数的名字、参数、返回值**；只新增了构造/析构和禁用拷贝（RAII），private 部分全部重写。

## 这一轮做了什么

把原来的内存假实现换成了真正的 SQLite 实现，数据落在 `dataDir/lexica.db`，和 C/E 共用同一个库。

- `open(dataDir)` 打开 `lexica.db`（只读方式打开，建库和迁移由 C 负责），校验 `vocab_item` 表存在，设 5 秒忙等超时；失败时 `db_` 为空，所有接口返回 `DbError`（`isPaused` 返回 false、`pausedCount` 返回 0），不会崩。
- 表用 C 已在**迁移 4** 登记的 `vocab_item` / `vocab_history` / `vocab_lookup_op`，没有新增迁移编号。
- 写操作都用 `BEGIN IMMEDIATE` 事务；`recordLookup` 靠 `vocab_lookup_op` 主键去重，`sqlite3_changes()==0` 说明是重试，直接提交并返回 `Ok`，不写第二条历史。
- 收藏、笔记、暂停通过 `INSERT OR IGNORE` + `UPDATE` 保证每个词只有一行。
- 笔记按 UTF-8 码点计数，超过 5000 返回 `BadRequest`，并且**先校验后动数据库**，原内容不变。
- `list` 在 SQL 里筛收藏/暂停，在内存里筛 `q`（不区分大小写子串）和 E 的阶段，再分页；`total` 是筛选后的总数，排序固定按 `entry_id`。
- `history` 只取 `kind='lookup'`，按 `seq DESC`（最近在前），用 `LIMIT/OFFSET` 分页。
- `stats` 用 `date(utc + 28800, 'unixepoch')` 按 Asia/Shanghai 分桶，固定返回 14 天、无活动补 0。
- `addReview` 只写一条 `kind='review'` 的历史（A 只在正式题、非重复提交后调用）。

### 关于「查词也进我的词汇」

`recordLookup` 会顺带在 `vocab_item` 里建一行（`ensureItem`）。这是按需求文档 **FR-12** 做的：

> 「我的词汇」包含**已接触或已收藏**的词；提供收藏、学习阶段、暂停状态及词头搜索条件。

所以「查过的词」和「收藏的词」都会出现在 `/api/vocab` 列表里；`fav=1` 只看收藏，`fav=0` 只看没收藏的。

## 怎么测的（全部实测通过）

### 1. 整机构建

用 GCC 16.2.0（MinGW-w64 UCRT）+ CMake/Ninja，`-std=c++17 -Wall -Wextra`：**零警告零错误**，链接出 `lexica.exe`。

### 2. 模块级测试 `vocab_tests.cpp`（73 项断言全通过）

这个文件没有接进 CMake，需要手动编译：

```sh
gcc -O2 -w -DSQLITE_OMIT_LOAD_EXTENSION -c third_party/sqlite3.c -o sqlite3.o
g++ -std=c++17 -Wall -Wextra -I a -I c -I d -I third_party -c \
    a/common/error.cpp a/common/clock.cpp c/dict/dict.cpp d/vocab/vocab.cpp d/vocab/vocab_tests.cpp
g++ *.o sqlite3.o -o vocab_tests -static
./vocab_tests
```

覆盖：

- 重复收藏只有一行；收藏/笔记/暂停/历史/统计重启后都还在
- 中文多行笔记（含换行、引号、制表符）原样往返
- 5000 字符通过、5001 字符拒绝且原内容不变；中文按码点算，5000 个汉字通过
- 同一个 `op_id` 重试只记一条，空 `op_id` 返回 `BadRequest`，重启后重试仍然只记一条
- 历史分页：4 条分 2 页，最近在前，页间不重叠
- 列表筛选：收藏 / 暂停 / 阶段（读 E 的 `stageOf`）/ `q` 不区分大小写；无匹配返回空页
- 分页：默认 20、上限 100、第二页取余数
- 统计：14 天、无活动补 0、按上海日期分桶（专门造了跨 UTC/上海日界的两行数据，确认落在上海日期而不是 UTC 日期）
- 没有数据库时所有接口优雅降级为 `DbError`

### 3. 端到端联调（真实 HTTP，43 项检查全通过）

用 `c/dict/testdata/ecdict-test.db` 起真实服务（`--port 8799 --data-dir <临时目录>`），分三个阶段：

- **A**：导入 → `dict_ready` 为 true；`lookup` 同一 `op_id` 重试只记 1 条；`ran` 回退到 `run`；`suggest` 不写历史；收藏两次只有一行；中文多行笔记原样往返；5001 字笔记返回 `BAD_REQUEST` 且旧内容不变；暂停；`q`/`stage`/分页筛选
- **B（杀进程重启）**：历史、收藏、笔记、暂停全部还在；重试旧 `op_id` 不新增历史；驱动一次真实复习（把 `review_state.due_utc` 置为过去）→ 提交正确 → `stats` 当天 `lookups=3`、`reviews=1`、`paused=1`、阶段分布合计 3
- **C（再次重启）**：`stats` 与历史完全保留

### 4. 边界与错误路径（全通过）

- 空库时 `stats` 返回 14 天全 0，`vocab`/`history` 为空列表
- 未导入时 `lookup` → `DICT_NOT_READY`；空词 / 缺 `op_id` → `BAD_REQUEST`；未命中 → `WORD_NOT_FOUND` 且**不写历史**
- `note` 缺 `text`、`favorite` 缺 `value` → `BAD_REQUEST`；对不存在的 id 收藏 → `WORD_NOT_FOUND`
- 5000 字通过、5001 字拒绝且保留 5000 字版本
- `page=0&size=0` 回落到默认；页码越界返回空 items 且 `total` 正确

## 需要 A 确认的改动

`CMakeLists.txt` 是 A 的文件，我为了让 `lexica_vocab` 能连 SQLite 改了一行：

```cmake
target_link_libraries(lexica_vocab PUBLIC lexica_common lexica_sqlite)
```

原来是 `lexica_common`。`lexica_sqlite` 已经把 `third_party` 作为 PUBLIC include 目录导出，所以 `d/vocab/vocab.cpp` 直接 `#include "sqlite3.h"` 就能用。

## 待确认的接口合同（两条，都不影响当前验收）

1. **`history` 是否按词去重。** 需求文档 FR-06 提到「建议首版显示每词最近一次历史，同时统计保留真实查询次数」，但又说「展示是否去重需固定为接口合同」，而 `docs/接口说明.md` 没有写。**当前实现是「不去重」**：每次正式查词都是一条事件，按时间倒序返回，`total` 是事件数（底层事件也全部保留，`stats` 的查词次数始终是真实次数）。如果 A/B 要「每词只显示最近一次」，说一声我改。
2. **`q` 的匹配语义。** 当前是**不区分大小写的子串匹配**（`q=ppl` 能命中 `apple`）。如果 B 的搜索框期望前缀匹配，告诉我，我改成前缀。

## 已知问题 / 备注

1. 上面那行 CMake 改动需要 A 合并；没合并的话 `d/vocab` 链接会失败。
2. D 的连接没有打开 `PRAGMA foreign_keys`，和 E 一致（C 那边是打开的）。因为 D 只写已经存在的词条，暂时没有影响；如果 A 希望严格外键约束，可以在 `open` 里加上，但那样传入不存在的 `entry_id` 会变成 `DbError`。
3. 单条 SQLite 连接在多个线程间共用（HTTP 服务有全局锁，E 也这么做）。如果以后去掉那把锁，需要给 D 的连接开 `SQLITE_OPEN_FULLMUTEX` 或加锁。
4. `vocab_tests.cpp` 依赖 `c/dict/dict.cpp` 建表（用它跑迁移），所以它算联调测试而不是纯单元测试。
5. `list` 目前把 `vocab_item` 全部读进内存再筛 `q` 和阶段。词表很大时可以考虑把 `q` 下推到 SQL；当前规模没有性能问题。
