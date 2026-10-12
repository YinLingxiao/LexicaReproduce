// ============================================================================
// review_test.cpp —— 任务 E 验收测试（新框架 SQLite 版）
// ----------------------------------------------------------------------------
// 覆盖（分派.txt §七 验收 + FR-14~19 关键规则）：
//   T1  重复提交只记一次（幂等，duplicate=true）+ 成绩落盘
//   T2  重练不改正式成绩、不推进阶段（Question.practice 标记）
//   T3  初始题目不泄露答案（英文释义出题，目标词遮住）
//   T4  用假时钟验证到期规则（1/3/7/14 天）
//   T5  阶段推进：答对无提示+1 / 答对用提示不变 / 答错-1（最低 0）
//   T5b 阶段降级（1→0）
//   T6  三级提示：下一级 / 重复不累计 / 第 3 级看答案判错
//   T7  单纯查词不推迟已有到期时间（ensureState 幂等）
//   T8  摘要：重练答对移出待巩固；重练仍错保留
// 编译（在仓库根目录，先编 sqlite3.o 再链接）：
//   gcc -c -O2 -Ithird_party -DSQLITE_OMIT_LOAD_EXTENSION third_party/sqlite3.c -o build/sqlite3.o
//   g++ -std=c++17 -O2 -Wall -Wextra -Ia -Ic -Id -Ie -Ithird_party ^
//       e/review/review.cpp c/dict/dict.cpp d/vocab/vocab.cpp ^
//       a/common/error.cpp a/common/clock.cpp e/review/review_test.cpp ^
//       build/sqlite3.o -o build/review_test.exe
// 运行：build\review_test.exe   （全部通过退出码 0）
// ============================================================================
#include "review/review.h"
#include "dict/dict.h"
#include "vocab/vocab.h"
#include "sqlite3.h"

#include <filesystem>
#include <iostream>
#include <string>

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

    explicit Env(const std::string& d) : dir("build/e_test/" + d) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        dict.open(dir);
        vocab.open(dir);
        review.open(dir);
        review.bind(&dict, &vocab);
        review.setClock(&clock);
        clock.set(1000000000LL);                       // 基准日 day0
        dict.importFile(fs::absolute("e/review/test_words.csv").string());
        // 导入 apple(1)/banana(2)/run(3)，英文释义出题
    }
    ~Env() {
        // 必须先关 SQLite 句柄再删目录，否则文件被占用无法删除
        review.close();
        dict.close();
        vocab.close();
        fs::remove_all(dir);
    }

    void at(std::int64_t day) { clock.set(1000000000LL + day * kDay); }

    // 会话里第一道未提交题（无题返回 0）
    QuestionId firstQid(const SessionId& sid) {
        auto n = review.next(sid);
        if (!n.ok() || !n.value.has) return 0;
        return n.value.question.id;
    }
};

// 成绩表行数（E 自己的表，直接查库验证落盘）
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

    std::cout << "==========================================\n";
    std::cout << "结果：" << gPass << " 通过，" << gFail << " 失败\n";
    return gFail == 0 ? 0 : 1;
}
