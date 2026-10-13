// ============================================================================
// review_test.cpp —— 复习模块验收测试（SQLite 版）
// ----------------------------------------------------------------------------
// 覆盖复习验收和需求文档 FR-14~19 的关键规则：
//   T1  重复提交只记一次（幂等，duplicate=true）+ 成绩落盘
//   T2  重练不改正式成绩、不推进阶段（Question.practice 标记）
//   T3  初始题目不泄露答案（英文释义出题，目标词遮住）
//   T4  用假时钟验证到期规则（1/3/7/14 天）
//   T5  阶段推进：答对无提示+1 / 答对用提示不变 / 答错-1（最低 0）
//   T5b 阶段降级（1→0）
//   T6  三级提示：下一级 / 重复不累计 / 第 3 级看答案判错
//   T7  单纯查词不推迟已有到期时间（ensureState 幂等）
//   T8  摘要：重练答对移出待巩固；重练仍错保留
// 编译：build.bat；运行：ctest --test-dir build --output-on-failure
// 也可从任意工作目录直接运行构建目录里的 review_test.exe。
// ============================================================================
#include "review/review.h"
#include "dict/dict.h"
#include "vocab/vocab.h"
#include "app/app.h"
#include "sqlite3.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---- 假时钟：测试时替换 systemClock（分派 §七.9）----
class FakeClock : public Clock {
public:
    void set(std::int64_t utc) { t_ = utc; }
    std::int64_t nowUtc() const override { return t_; }
private:
    std::int64_t t_ = 0;
};

// ---- 极简断言框架 ----
static int gPass = 0, gFail = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { ++gPass; std::cout << "  [PASS] " << name << "\n"; }     \
        else     { ++gFail; std::cout << "  [FAIL] " << name << "\n"; }      \
    } while (0)

namespace {

constexpr std::int64_t kDay = 86400;

struct Env {
    Dict dict;
    Vocab vocab;
    Review review;
    FakeClock clock;
    std::string dir;

    explicit Env(const std::string& d) : dir(test_support::freshDirectory("review", d).u8string()) {
        dict.open(dir);
        vocab.open(dir);
        review.open(dir);
        review.bind(&dict, &vocab);
        review.setClock(&clock);
        clock.set(1000000000LL);                       // 基准日 day0
        dict.importFile(test_support::dataFile("test_words.csv").u8string());
        // 导入 apple(1)/banana(2)/run(3)，英文释义出题
    }
    ~Env() {
        // 必须先关 SQLite 句柄再删目录，否则文件被占用无法删除
        review.close();
        dict.close();
        vocab.close();
        test_support::removeDirectory(fs::u8path(dir));
    }

    void at(std::int64_t day) { clock.set(1000000000LL + day * kDay); }

    // 会话里第一道未提交题（无题返回 0）
    QuestionId firstQid(const SessionId& sid) {
        auto n = review.next(sid);
        if (!n.ok() || !n.value.has) return 0;
        return n.value.question.id;
    }
};

// 成绩表行数（复习模块 自己的表，直接查库验证落盘）
int resultRows(const std::string& dir, bool practiceOnly) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2((dir + "/lexica.db").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return -1;
    const char* sql = practiceOnly
        ? "SELECT COUNT(*) FROM review_result WHERE practice=1"
        : "SELECT COUNT(*) FROM review_result";
    sqlite3_stmt* st = nullptr;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

} // namespace

// ============================ T1 幂等 + 成绩落盘 ============================
static void testIdempotent(Env& env) {
    std::cout << "T1 重复提交只记一次（幂等）且成绩落盘\n";
    env.at(0);
    CHECK(env.review.ensureState(1) == ErrorCode::Ok, "初始化 apple 状态");

    env.at(1);
    auto start = env.review.start();
    CHECK(start.ok() && !start.value.empty(), "day1 能开始会话");
    QuestionId qid = env.firstQid(start.value);
    CHECK(qid != 0, "能取到正式题");

    auto r1 = env.review.submit(qid, "Apple");          // 大小写混合
    CHECK(r1.ok() && r1.value.correct && !r1.value.duplicate, "首次提交判对");
    auto r2 = env.review.submit(qid, "apple");           // 重复提交
    CHECK(r2.ok() && r2.value.duplicate && r2.value.correct == r1.value.correct,
          "重复提交 duplicate=true 且结果一致");
    CHECK(env.review.stageOf(1) == 1, "重复提交不重复推进阶段");
    CHECK(resultRows(env.dir, false) == 1, "成绩表只有 1 条（重复提交不重复记）");

    Review reload;                                       // 重启恢复
    reload.open(env.dir);
    reload.setClock(&env.clock);
    CHECK(reload.stageOf(1) == 1, "重启后阶段恢复");
}

// ============================ T2 重练不改正式成绩 ============================
static void testPracticeKeepsFormal(Env& env) {
    std::cout << "T2 重练不改正式成绩、不推进阶段\n";
    env.at(0);
    env.review.ensureState(1);
    env.review.ensureState(2);

    env.at(1);
    auto start = env.review.start();
    QuestionId q1 = env.firstQid(start.value);           // apple(1)
    auto w = env.review.submit(q1, "wrong");
    CHECK(w.ok() && !w.value.correct, "q1 正式答错");

    auto n2 = env.review.next(start.value);              // banana(2) 未提交
    CHECK(n2.ok() && n2.value.has && n2.value.question.entryId == 2, "下一题是 banana");
    auto r = env.review.submit(n2.value.question.id, "banana");
    CHECK(r.ok() && r.value.correct && !r.value.practice, "q2 正式答对（非重练）");

    auto p = env.review.next(start.value);               // apple 的重练题
    CHECK(p.ok() && p.value.has && p.value.question.practice, "能取到重练题（practice=true）");
    CHECK(p.value.question.entryId == 1, "重练题是答错的 apple");
    auto pr = env.review.submit(p.value.question.id, "apple");
    CHECK(pr.ok() && pr.value.correct && pr.value.practice, "重练提交判对");

    CHECK(env.review.stageOf(1) == 0, "重练答对不推进阶段（仍 0）");
    CHECK(env.review.stageOf(2) == 1, "正式答对无提示 → 阶段 1");

    auto sum = env.review.summary(start.value);
    CHECK(sum.ok() && sum.value.formalCount == 2 && sum.value.formalCorrect == 1,
          "正式成绩：2 题对 1（重练不改变正式统计）");
    CHECK(sum.value.practiceCount == 1, "重练已答 1 题");
    CHECK(sum.value.reinforce.empty(), "重练答对后不再列为待巩固");
    CHECK(resultRows(env.dir, true) == 1, "成绩表 practice=1 共 1 条");
}

// ============================ T3 不泄露答案 ============================
static void testNoAnswerLeak(Env& env) {
    std::cout << "T3 初始题目不泄露答案\n";
    env.at(0);
    env.review.ensureState(1);
    env.review.ensureState(2);
    env.review.ensureState(3);

    env.at(1);
    auto start = env.review.start();
    bool allHidden = true;
    bool allLocal = true;
    int checked = 0;
    int formal = 0;                                      // 只数正式题（答错的题会生成重练题）
    auto n = env.review.next(start.value);
    while (n.ok() && n.value.has && checked < 20) {
        const Question& q = n.value.question;
        if (q.kind != "local") allLocal = false;         // 未注入 AI → 全部本地题
        if (!q.practice) ++formal;
        std::string lower = q.prompt;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string answer;
        if (q.entryId == 1) answer = "apple";
        else if (q.entryId == 2) answer = "banana";
        else answer = "run";
        if (lower.find(answer) != std::string::npos) allHidden = false;
        env.review.submit(q.id, "x");                    // 提交推进队列
        ++checked;
        n = env.review.next(start.value);
    }
    CHECK(allLocal, "未注入 AI 例句时全部出 local 题");
    CHECK(allHidden, "所有题干均不含目标单词（英文释义出题且已遮词）");
    CHECK(formal == 3, "会话共 3 道正式题");
}

// ============================ T4 到期规则 ============================
static void testDueRule(Env& env) {
    std::cout << "T4 用假时钟验证到期规则\n";
    env.at(0);
    env.review.ensureState(1);                           // due = day0 + 1

    auto s0 = env.review.start();
    CHECK(env.firstQid(s0.value) == 0, "day0 无到期词（新词 1 天后到期）");

    env.at(1);
    auto s1 = env.review.start();
    QuestionId qid = env.firstQid(s1.value);
    CHECK(qid != 0, "day1 到期并选中");

    auto r = env.review.submit(qid, "apple");            // 答对无提示
    CHECK(r.ok() && r.value.stage == 1, "答对无提示 → 阶段 1");

    env.at(2);
    auto s2 = env.review.start();
    CHECK(env.firstQid(s2.value) == 0, "day2 未到期（stage1 间隔 3 天）");

    env.at(4);
    auto s4 = env.review.start();
    CHECK(env.firstQid(s4.value) != 0, "day4 stage1 到期（+3 天）");
}

// ============================ T5 阶段推进 ============================
static void testStageRules(Env& env) {
    std::cout << "T5 阶段推进：对无提示+1 / 对用提示不变 / 错-1（最低 0）\n";
    env.at(0);
    env.review.ensureState(1);
    env.review.ensureState(2);
    env.review.ensureState(3);

    env.at(1);
    auto start = env.review.start();
    QuestionId q1 = env.firstQid(start.value);           // apple
    env.review.submit(q1, "apple");                      // 对、无提示
    auto n2 = env.review.next(start.value); QuestionId q2 = n2.value.question.id; // banana
    env.review.hint(q2, 1);
    env.review.submit(q2, "banana");                     // 对、用提示
    auto n3 = env.review.next(start.value); QuestionId q3 = n3.value.question.id; // run
    env.review.submit(q3, "wrong");                      // 错

    CHECK(env.review.stageOf(1) == 1, "答对无提示：0→1");
    CHECK(env.review.stageOf(2) == 0, "答对但用过提示：0→0（不变）");
    CHECK(env.review.stageOf(3) == 0, "答错：0→0（最低 0，不降为负）");
}

// 降级单独验证（独立环境，避免其它到期词干扰选题）
static void testStageDowngrade(Env& env) {
    std::cout << "T5b 阶段降级：答错降一级（1→0）\n";
    env.at(0);
    env.review.ensureState(1);

    env.at(1);
    auto s1 = env.review.start();
    QuestionId q = env.firstQid(s1.value);
    auto r = env.review.submit(q, "apple");
    CHECK(r.ok() && r.value.stage == 1, "答对无提示 → stage1");

    env.at(4);
    auto s4 = env.review.start();
    QuestionId q2 = env.firstQid(s4.value);
    CHECK(q2 != 0, "day4 apple 到期可复习");
    auto r2 = env.review.submit(q2, "bad");
    CHECK(r2.ok() && !r2.value.correct && r2.value.stage == 0, "答错 → stage 1→0");
}

// ============================ T6 三级提示 ============================
static void testHints(Env& env) {
    std::cout << "T6 三级提示：下一级 / 重复不累计 / 看答案判错\n";
    env.at(0);
    env.review.ensureState(1);                           // apple: definition="a round fruit"

    env.at(1);
    auto start = env.review.start();
    QuestionId qid = env.firstQid(start.value);

    auto h1 = env.review.hint(qid, 0);                   // level=0 → 下一级（1）
    CHECK(h1.ok() && h1.value.level == 1, "level=0 取到第 1 级");
    CHECK(h1.value.text == "a____", "第 1 级=首字母+掩码（a____）");

    auto h2 = env.review.hint(qid, 0);                   // 下一级（2）
    CHECK(h2.ok() && h2.value.level == 2, "第 2 级=另一种释义");
    CHECK(h2.value.text == "n. 苹果", "题干英文 → 第 2 级给中文释义");

    auto h3 = env.review.hint(qid, 0);                   // 下一级（3）
    CHECK(h3.ok() && h3.value.level == 3 && h3.value.text == "apple",
          "第 3 级=直接给答案");

    auto hRep = env.review.hint(qid, 1);                 // 重复请求低级
    CHECK(hRep.ok() && hRep.value.level == 1, "重复请求返回指定级文本");
    auto hNext = env.review.hint(qid, 0);
    CHECK(hNext.ok() && hNext.value.level == 3, "已到最高级，level=0 不再上升");

    auto r = env.review.submit(qid, "apple");            // 看过答案仍答对
    CHECK(r.ok() && !r.value.correct, "看过答案（第 3 级）提交判错");
    CHECK(r.value.hintLevel == 3, "hintLevel 回传供前端展示");
}

// ============================ T7 查词不推迟到期 ============================
static void testEnsureKeepsDue(Env& env) {
    std::cout << "T7 单纯查词不推迟已有到期时间\n";
    env.at(0);
    env.review.ensureState(1);                           // due = day0 + 1
    env.at(1);
    env.review.ensureState(1);                           // 已存在 → 不动
    auto start = env.review.start();
    CHECK(env.firstQid(start.value) != 0, "day1 仍到期（due 未被查词推迟）");
}

// ============================ T8 摘要与待巩固 ============================
static void testSummary(Env& env) {
    std::cout << "T8 摘要：重练答对移出待巩固 / 重练仍错保留\n";
    env.at(0);
    env.review.ensureState(1);
    env.review.ensureState(2);

    env.at(1);
    auto start = env.review.start();
    QuestionId q1 = env.firstQid(start.value);           // apple
    env.review.submit(q1, "x");
    auto n2 = env.review.next(start.value); QuestionId q2 = n2.value.question.id; // banana
    env.review.submit(q2, "y");

    auto p1 = env.review.next(start.value);              // apple 重练
    CHECK(p1.ok() && p1.value.has && p1.value.question.entryId == 1, "取到第 1 道重练题");
    env.review.submit(p1.value.question.id, "apple");    // 重练答对
    auto p2 = env.review.next(start.value);              // banana 重练
    CHECK(p2.ok() && p2.value.has && p2.value.question.entryId == 2, "取到第 2 道重练题");
    env.review.submit(p2.value.question.id, "still wrong");

    auto sum = env.review.summary(start.value);
    CHECK(sum.ok() && sum.value.formalCount == 2 && sum.value.formalCorrect == 0,
          "正式 2 题全错（重练不计正式成绩）");
    CHECK(sum.value.practiceCount == 2, "重练已答 2 题");
    CHECK(sum.value.reinforce.size() == 1 && sum.value.reinforce[0] == "banana",
          "待巩固只剩重练仍错的 banana");
}

bool execSql(const std::string& dir, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2((dir + "/lexica.db").c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 5000);
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

int scalar(const std::string& dir, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2((dir + "/lexica.db").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt* st = nullptr;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

int reviewEvents(const std::string& dir) { return scalar(dir, "SELECT COUNT(*) FROM vocab_history WHERE kind='review'"); }

// ============================ 成绩落盘失败后可以重试 ============================
static void testPersistRetry(Env& env) {
    std::cout << "T9 成绩写入失败不记判分，重试仍可成功\n";
    env.at(0);
    env.review.ensureState(1);
    env.at(1);
    auto start = env.review.start();
    QuestionId q = env.firstQid(start.value);
    CHECK(execSql(env.dir, "CREATE TRIGGER review_result_block BEFORE INSERT ON review_result "
                           "BEGIN SELECT RAISE(ABORT, 'block'); END;"),
          "安装成绩触发器");
    auto failed = env.review.submit(q, "apple");
    CHECK(!failed.ok() && failed.code == ErrorCode::DbError, "正式题落盘失败返回 DbError");
    CHECK(env.review.stageOf(1) == 0, "失败后阶段仍是 0");
    CHECK(resultRows(env.dir, false) == 0 && reviewEvents(env.dir) == 0, "失败后成绩和复习次数都没有写入");
    CHECK(env.firstQid(start.value) == q, "失败后原题还能再答");
    CHECK(execSql(env.dir, "DROP TRIGGER review_result_block;"), "移除成绩触发器");
    auto ok = env.review.submit(q, "apple");
    CHECK(ok.ok() && ok.value.correct && !ok.value.duplicate && ok.value.stage == 1, "正式题重试成功且不是重复提交");
    CHECK(env.review.stageOf(1) == 1 && resultRows(env.dir, false) == 1 && reviewEvents(env.dir) == 1, "重试只升级一次并记下一次复习");
    auto dup = env.review.submit(q, "other");
    CHECK(dup.ok() && dup.value.duplicate && dup.value.stage == 1, "再次提交是重复结果");
    CHECK(env.review.stageOf(1) == 1 && resultRows(env.dir, false) == 1 && reviewEvents(env.dir) == 1, "重复提交不升级也不加复习次数");

    env.review.ensureState(2);
    env.at(2);
    auto second = env.review.start();
    QuestionId banana = env.firstQid(second.value);
    auto wrong = env.review.submit(banana, "no");
    CHECK(wrong.ok() && !wrong.value.correct && !wrong.value.practice, "banana 正式答错");
    auto practice = env.review.next(second.value);
    CHECK(practice.ok() && practice.value.has && practice.value.question.practice, "生成重练题");
    QuestionId pq = practice.value.question.id;
    const int stageBefore = env.review.stageOf(2);
    const int formalBefore = resultRows(env.dir, false);
    CHECK(execSql(env.dir, "CREATE TRIGGER review_result_block BEFORE INSERT ON review_result "
                           "BEGIN SELECT RAISE(ABORT, 'block'); END;"),
          "重练前重新安装成绩触发器");
    auto practiceFail = env.review.submit(pq, "banana");
    CHECK(!practiceFail.ok() && practiceFail.code == ErrorCode::DbError, "重练落盘失败返回 DbError");
    CHECK(env.review.stageOf(2) == stageBefore && resultRows(env.dir, true) == 0, "失败的重练不改阶段也不写成绩");
    CHECK(env.review.next(second.value).value.question.id == pq, "失败后重练题还能再答");
    CHECK(execSql(env.dir, "DROP TRIGGER review_result_block;"), "移除重练触发器");
    auto practiceOk = env.review.submit(pq, "banana");
    CHECK(practiceOk.ok() && practiceOk.value.practice && practiceOk.value.correct && !practiceOk.value.duplicate, "重练重试成功");
    CHECK(env.review.stageOf(2) == stageBefore && resultRows(env.dir, true) == 1, "重练重试不推进阶段");
    CHECK(resultRows(env.dir, false) == formalBefore + 1 && reviewEvents(env.dir) == 2, "重练不增加正式复习次数");
    auto practiceDup = env.review.submit(pq, "banana");
    CHECK(practiceDup.ok() && practiceDup.value.duplicate && resultRows(env.dir, true) == 1 && reviewEvents(env.dir) == 2,
          "重练重复提交不再写成绩");
}

// ============================ 同一到期状态只能计一次正式分 ============================
static void testOneDueState(Env& env) {
    std::cout << "T10 两轮不能把同一次到期连续升级\n";
    env.at(0);
    env.review.ensureState(1);
    env.at(1);
    auto first = env.review.start();
    QuestionId oldQ = env.firstQid(first.value);
    auto second = env.review.start();
    QuestionId newQ = env.firstQid(second.value);
    CHECK(oldQ != 0 && newQ != 0 && oldQ != newQ, "连续两轮都曾包含 apple");
    auto expired = env.review.submit(oldQ, "apple");
    CHECK(!expired.ok() && expired.code == ErrorCode::QuestionExpired, "旧正式题已失效");
    CHECK(env.review.stageOf(1) == 0 && resultRows(env.dir, false) == 0, "失效题不计分");
    auto graded = env.review.submit(newQ, "apple");
    CHECK(graded.ok() && graded.value.correct && !graded.value.duplicate && graded.value.stage == 1, "新一轮只升到阶段 1");
    CHECK(env.review.stageOf(1) == 1 && resultRows(env.dir, false) == 1 && reviewEvents(env.dir) == 1, "只保存一条正式成绩");
    auto dup = env.review.submit(newQ, "apple");
    CHECK(dup.ok() && dup.value.duplicate && env.review.stageOf(1) == 1 && resultRows(env.dir, false) == 1, "新题重复提交不再升级");

    env.review.ensureState(2);
    env.at(2);
    auto oldSession = env.review.start();
    QuestionId oldBanana = env.firstQid(oldSession.value);
    auto newSession = env.review.start();
    QuestionId newBanana = env.firstQid(newSession.value);
    auto wrong = env.review.submit(newBanana, "no");
    CHECK(wrong.ok() && !wrong.value.correct && !wrong.value.practice, "新一轮正式答错");
    CHECK(env.review.submit(oldBanana, "banana").code == ErrorCode::QuestionExpired, "旧正式题不能再答对升级");
    auto practice = env.review.next(newSession.value);
    CHECK(practice.ok() && practice.value.has && practice.value.question.practice && practice.value.question.entryId == 2, "重练仍在新一轮");
    auto practiced = env.review.submit(practice.value.question.id, "banana");
    CHECK(practiced.ok() && practiced.value.practice && practiced.value.correct, "重练可以正常提交");
    CHECK(env.review.stageOf(2) == 0 && resultRows(env.dir, true) == 1 && reviewEvents(env.dir) == 2, "重练不升阶段也不加正式次数");
}

static void testStaleDue(Env& env) {
    std::cout << "T11 提交时学习状态版本不符则失效\n";
    env.at(0);
    env.review.ensureState(1);
    env.at(1);
    auto start = env.review.start();
    QuestionId q = env.firstQid(start.value);
    CHECK(execSql(env.dir, "UPDATE review_state SET due_utc = due_utc + 1 WHERE entry_id = 1"), "改动到期时间");
    auto stale = env.review.submit(q, "apple");
    CHECK(!stale.ok() && stale.code == ErrorCode::QuestionExpired, "过期状态不能计分");
    CHECK(env.review.stageOf(1) == 0 && resultRows(env.dir, false) == 0 && reviewEvents(env.dir) == 0, "版本不符不写成绩");
    auto next = env.review.next(start.value);
    CHECK(next.ok() && !next.value.has, "失效题不再作为未答题返回");
}

// ============================ 统计补记 ============================
static void testHistoryBackfill() {
    std::cout << "T12 复习次数写入失败可在重复提交时补记\n";
    const std::string dir = test_support::freshDirectory("review", "app_review").u8string();
    App app;
    CHECK(app.start(dir) == StartupStatus::DictEmpty, "应用可以启动");
    CHECK(app.importDict(test_support::dataFile("test_words.csv").u8string()).ok(), "导入测试词库");
    auto apple = app.lookup("apple", "op-apple");
    CHECK(apple.ok(), "查到 apple");
    const std::string makeDue = "UPDATE review_state SET due_utc=0 WHERE entry_id=" + std::to_string(apple.value.id);
    CHECK(execSql(dir, makeDue.c_str()), "让 apple 到期");
    CHECK(execSql(dir, "CREATE TRIGGER vocab_review_block BEFORE INSERT ON vocab_history WHEN NEW.kind='review' "
                       "BEGIN SELECT RAISE(ABORT, 'block'); END;"),
          "阻止写入复习次数");
    auto sid = app.reviewStart();
    auto question = app.reviewNext(sid.value);
    CHECK(sid.ok() && question.ok() && question.value.has, "取到 apple 正式题");
    const QuestionId qid = question.value.question.id;
    auto failed = app.reviewSubmit(qid, "apple");
    CHECK(!failed.ok() && failed.code == ErrorCode::DbError, "次数写失败时提交返回 DbError");
    CHECK(scalar(dir, "SELECT stage FROM review_state") == 0, "失败后阶段仍是 0");
    CHECK(scalar(dir, "SELECT COUNT(*) FROM review_result") == 0 && reviewEvents(dir) == 0, "失败后成绩和次数都没写");
    auto retryQuestion = app.reviewNext(sid.value);
    CHECK(retryQuestion.ok() && retryQuestion.value.has && retryQuestion.value.question.id == qid, "失败后题目还能提交");
    CHECK(execSql(dir, "DROP TRIGGER vocab_review_block;"), "恢复复习次数写入");
    auto graded = app.reviewSubmit(qid, "apple");
    CHECK(graded.ok() && graded.value.correct && !graded.value.duplicate && graded.value.stage == 1, "重试得到真实成功");
    CHECK(scalar(dir, "SELECT stage FROM review_state") == 1 && scalar(dir, "SELECT COUNT(*) FROM review_result") == 1 && reviewEvents(dir) == 1,
          "重试后成绩和次数一致");
    auto dup = app.reviewSubmit(qid, "apple");
    CHECK(dup.ok() && dup.value.duplicate && reviewEvents(dir) == 1, "重复提交不增加次数");
    CHECK(execSql(dir, "DELETE FROM vocab_history WHERE kind='review'"), "删掉已写入的复习次数");
    auto filled = app.reviewSubmit(qid, "apple");
    CHECK(filled.ok() && filled.value.duplicate && reviewEvents(dir) == 1, "重复提交补上缺失的一次");
    auto filledAgain = app.reviewSubmit(qid, "apple");
    CHECK(filledAgain.ok() && filledAgain.value.duplicate && reviewEvents(dir) == 1, "补记之后不再增加");

    auto banana = app.lookup("banana", "op-banana");
    CHECK(banana.ok(), "查到 banana");
    const std::string bananaDue = "UPDATE review_state SET due_utc=0 WHERE entry_id=" + std::to_string(banana.value.id);
    CHECK(execSql(dir, bananaDue.c_str()), "让 banana 到期");
    auto sid2 = app.reviewStart();
    auto formal = app.reviewNext(sid2.value);
    CHECK(formal.ok() && formal.value.has && formal.value.question.entryId == banana.value.id && !formal.value.question.practice, "banana 是正式题");
    auto wrong = app.reviewSubmit(formal.value.question.id, "no");
    CHECK(wrong.ok() && !wrong.value.correct && !wrong.value.practice && reviewEvents(dir) == 2, "正式答错记一次复习");
    auto practice = app.reviewNext(sid2.value);
    CHECK(practice.ok() && practice.value.has && practice.value.question.practice, "进入重练");
    auto practiced = app.reviewSubmit(practice.value.question.id, "banana");
    CHECK(practiced.ok() && practiced.value.practice && practiced.value.correct && reviewEvents(dir) == 2, "重练不增加正式次数");
    auto practiceDup = app.reviewSubmit(practice.value.question.id, "banana");
    CHECK(practiceDup.ok() && practiceDup.value.duplicate && reviewEvents(dir) == 2, "重练重复提交不增加次数");
    app.shutdown();
    test_support::removeDirectory(fs::u8path(dir));
}

// ============================ 主函数 ============================
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // Windows 控制台 UTF-8 显示
#endif
    std::cout << "任务 E（新框架 SQLite 版）验收测试开始\n";
    std::cout << "==========================================\n";

    Env e1("t1"); testIdempotent(e1);
    Env e2("t2"); testPracticeKeepsFormal(e2);
    Env e3("t3"); testNoAnswerLeak(e3);
    Env e4("t4"); testDueRule(e4);
    Env e5("t5"); testStageRules(e5);
    Env e6("t5b"); testStageDowngrade(e6);
    Env e7("t6"); testHints(e7);
    Env e8("t7"); testEnsureKeepsDue(e8);
    Env e9("t8"); testSummary(e9);
    Env e10("t9"); testPersistRetry(e10);
    Env e11("t10"); testOneDueState(e11);
    Env e12("t11"); testStaleDue(e12);
    testHistoryBackfill();

    std::cout << "==========================================\n";
    std::cout << "结果：" << gPass << " 通过，" << gFail << " 失败\n";
    return gFail == 0 ? 0 : 1;
}
