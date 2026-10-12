# E 复习与记忆调度模块 —— 实现思路

> 目录：`e/review/`　负责人：E　基座：A 的 SQLite 版（commit 87c6fea "A/C/E：词库、复习、AI 接入真实实现"）

## 一、模块定位

本目录是五人项目里 E 的交付范围。A 已在 10-10 把仓库重构为 `a/ b/ c/ d/ e/`，并合入了可用的 SQLite 版复习模块。**E 的活是在这个基座上补齐三个缺口**，不是另起炉灶：

| 缺口 | 分派依据 | A 版现状 | 本版做法 |
|---|---|---|---|
| ① 可注入时钟 | §七.9「时间用 Clock，测试时换假时钟」 | 全部直接调 `systemClock()` | 新增 `setClock(const Clock*)`，内部统一走 `nowUtc()`（注入则用注入的，否则回退系统时钟） |
| ② 成绩持久化 | §七.7「成绩和学习状态在同一个事务里保存」 | 只写了 `review_state`（阶段/到期时间），成绩丢了 | 新增 `review_result` 成绩表，正式题与重练题的成绩都落盘 |
| ③ 验收测试 | 分派验收四条 | 无 | `review_test.cpp`：49 条断言全绿（假时钟驱动，不用真等几天） |

对外**公开函数签名与 `Question / NextQuestion / Hint / Grade / Summary` 字段全部保持 A 版原样**，A 的 `app.cpp` 一行不用改。

## 二、整体数据流

```
A 启动:
  dict.open(dataDir) ──► C 的迁移建好 review_state(entry_id, stage, due_utc)
  vocab.open(dataDir)
  review.open(dataDir)  ──► 检查 review_state 存在；自建 review_result 成绩表
  review.bind(&dict, &vocab)
  review.setSentenceSource(AI 例句回调)   // kind="ai" 题源，失败自动回退本地题

用户查词:
  review.ensureState(id)   ──► 第一次见词插 (stage=0, due=now+1天)；已存在则不动
                                （单纯查词绝不推迟已算好的到期时间）

用户点“开始复习”:
  review.start() ──► 查 review_state 里 due<=now 的词
                     ──► 跳过 vocab_->isPaused 的词、没释义的词
                     ──► 按 (due_utc, entry_id) 排序，每轮最多 12 道正式题

一轮会话（全部在内存，题目对象挂在 Session 上）:
  next  ──► 取第一道未提交、未暂停的题
  hint  ──► 三级提示：1 首字母+掩码 → 2 另一种释义 → 3 直接给答案
  submit──► 判分 + 改阶段 + 成绩落盘（同事务） + 答错自动生成重练题
  summary──► 正式题统计 / 重练统计 / 待巩固列表
```

## 三、关键设计

### 1. 时钟注入（缺口①）
- `Clock` 是 `a/common/clock.h` 里的抽象（`virtual int64_t nowUtc() const`），生产用 `systemClock()` 单例。
- 新增公开函数 `void setClock(const Clock*)`：**只存指针，不改任何既有签名**，A 不调用也不受影响。
- 所有时间点（ensureState 的初始到期、start 的到期判断、submit 的下次到期）一律走私有 `nowUtc()`：
  `clock_ ? clock_->nowUtc() : systemClock().nowUtc()`。
- 测试里用 `FakeClock` 逐天拨时间，几毫秒内验证「1/3/7/14 天到期」而不用真等。

### 2. 成绩表与同事务（缺口②）
- 新表（`review_result`）在 `Review::open()` 里 **`CREATE TABLE IF NOT EXISTS` 幂等自建**：
  `qid / entry_id / session_id / correct / practice / utc`。
- **为什么不自建迁移版本号**：C 的 `dict.cpp` 用 `schema_migrations(module, version)` 管理版本，且启动时校验 `reviewVersion > kReviewSchemaVersion` 会直接判 `VersionTooNew` 拒绝开库。E 若往 `schema_migrations` 登记 review v2，C 下次打开数据库会认为版本太新 → 整个程序起不来。所以 E 的表**不登记迁移**，只在 open 时幂等建表。
  - 代价：如果将来要统一迁移管理，需要 **C 在 dict.cpp 里登记 review v2**（本 README 已注明，交付时向 C 说明）。
- 事务（`persist()`，对应分派 §七.7「同一事务」）：
  - `BEGIN IMMEDIATE`（立即拿写锁，防止并发写穿插）
  - 正式题：`UPDATE review_state`（新阶段/新到期） **＋** `INSERT review_result`（correct 按是否掌握）
  - 重练题：只 `INSERT review_result`（practice=1），**不碰 review_state**（§七.6 重练不改正式成绩/阶段）
  - 任一步失败 → `ROLLBACK` 并返回 `DbError`，保证状态和成绩要么都写、要么都不写。

### 3. 选题规则（§七.1）
`SELECT entry_id FROM review_state WHERE due_utc<=? ORDER BY due_utc, entry_id` → 逐词过滤：
已暂停（`vocab_->isPaused`）跳过、无释义（`definition/translation` 都空）跳过、`buildItem` 失败（出不了干净题）跳过；凑满 `kReviewFormalMax=12` 就停。

### 4. 出题与遮词（§七.2，题目不泄露答案）
- 取词的英文释义作题干（没英文用中文），把题干/另一种释义里出现的**目标词及其词形**（`dict_->forms`，按长度降序）遮成下划线：整词边界匹配、大小写不敏感（如 `a round fruit` 对 `apple` 无词可遮，题干就是干净的；`apples are red` 会变成 `______ are red`）。
- `buildItem` 末尾有兜底：题干里若还残留目标词 → 弃题不出，绝不把答案带进题干。
- AI 例句（kind="ai"，§七.8）：`setSentenceSource` 有值且例句遮词后干净 → 用例句；否则自动回退本地题（kind="local"）。

### 5. 提示三级（§七.3，按接口清单口径）
`hint(qid, level)`：`level=0` 表示「取下一级」，已到第 3 级不再上升；
`item.hintLevel` 只保存**最高**请求过的级别，重复请求低级不往回降（也不重复累计）。
- 第 1 级：首字母 + 下划线（`a____`）
- 第 2 级：另一种释义（题干英文 → 给中文，反之亦然）
- 第 3 级：直接给答案

### 6. 判分与阶段（§七.4/5）
- 判分：`normalizeWord(answer) == normalizeWord(item.answer)` —— 去首尾空白、不分大小写、严格匹配。
- 掌握 = 判对 **且** 没用过第 3 级提示（看过答案按没掌握算）。
- 阶段迁移（0~3，间隔 1/3/7/14 天）：
  - 答对 + 没用任何提示 → `stage+1`（上限 3）
  - 答对但用过提示 → 阶段不变
  - 答错 → `stage-1`（下限 0）
  - 第一次接触 = 阶段 0，1 天后到期；查词不推迟已有到期。

### 7. 错词重练与摘要（§七.6）
- 正式题答错 → 在本轮会话末尾追加一道该词的重练题（同词不重复追加）。
- 重练提交：判分照常，但**不写 review_state**，只记 `review_result(practice=1)`，不改变正式成绩和阶段。
- `summary`：`formalCount/formalCorrect` 只统计已提交的正式题；`practiceCount` 统计重练提交次数；`reinforce` = 正式答错且重练没答对的词（重练答对就移出待巩固）。

### 8. 幂等（验收第一条）
`submit` 对已提交过的题返回**同一份结果**并置 `duplicate=true`；成绩只在首次提交时写入，重复提交不新增记录。

## 四、验收测试

文件：`review_test.cpp`（测试代码，不参与产品构建）+ `test_words.csv`（测试词库，apple/banana/run 三词）。

### 编译与运行（在仓库根目录）
```bat
:: 第一次先编 sqlite 对象（已存在可跳过）
gcc -c -O2 -Ithird_party -DSQLITE_OMIT_LOAD_EXTENSION third_party/sqlite3.c -o build/sqlite3.o
g++ -std=c++17 -O2 -Wall -Wextra -Ia -Ic -Id -Ie -Ithird_party ^
    e/review/review.cpp c/dict/dict.cpp d/vocab/vocab.cpp ^
    a/common/error.cpp a/common/clock.cpp e/review/review_test.cpp ^
    build/sqlite3.o -o build/review_test.exe
build\review_test.exe
```
全部通过退出码为 0，输出 `49 通过，0 失败`。测试自建 `build/e_test/` 临时数据目录、测完自动清理。

### 覆盖的验收点
| 验收要求 | 对应测试 |
|---|---|
| 重复提交只记一次（成绩表仅 1 条、阶段不重复推进、重启后阶段仍在） | T1 |
| 重练不改正式成绩（阶段不动、正式统计不含重练、成绩表 practice 标记） | T2 / T8 |
| 初始题目不泄露答案（题干不含目标词；未接 AI 时全为 local 题） | T3 |
| 用测试时间验证到期规则（新词 1 天、stage1 间隔 3 天；未到期不出现） | T4 / T7 |
| 阶段迁移、降级、三级提示、看答案判错 | T5 / T5b / T6 |

## 五、文件清单与交接说明

| 文件 | 说明 |
|---|---|
| `review.h` | 公开合同（A 版原样）+ 新增 `setClock` |
| `review.cpp` | 实现：时钟注入 + 成绩表同事务 + 原 A 版全部规则 |
| `review_test.cpp` | 49 断言验收测试 |
| `test_words.csv` | 测试词库（5 列格式与 C 的导入器一致，**exchange 列必填**） |

**建议交接给 A**（沿用分派 §八流程：`git checkout -b e/clock-persist-test` → commit → push → PR）：
1. 做了什么：三缺口（假时钟 / 成绩表 `review_result` 同事务 / 49 条验收测试），公开接口零改动；
2. 怎么测的：`build\review_test.exe` 49 通过；`build.bat` 全量编译通过（lexica.exe 正常链接）；
3. 测试数据：`e/review/test_words.csv` 三词 + `build/e_test/`（自动清理）；
4. 已知问题：
   - `review_result` 未纳入 C 的 `schema_migrations`（避免 `VersionTooNew`），如需统一迁移请 C 登记 review v2；
   - 会话（进行中的一轮题）存在内存，进程重启后旧会话失效，但已提交的成绩/状态均已落盘；
   - AI 例句依赖 A 注入的 `setSentenceSource`，例句遮词不干净时会回退本地题（A 版既有行为，保留）。
