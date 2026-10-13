// D 模块（个人词汇与统计）验证程序。
//
// 用法（本文件不接入 CMake，独立编译）：
//   gcc -O2 -w -DSQLITE_OMIT_LOAD_EXTENSION -c third_party/sqlite3.c -o sqlite3.o
//   g++ -std=c++17 -I a -I c -I d -I third_party -c a/common/error.cpp a/common/clock.cpp
//       c/dict/dict.cpp d/vocab/vocab.cpp d/vocab/vocab_tests.cpp
//   g++ *.o sqlite3.o -o vocab_tests -static
//   ./vocab_tests
//
// 覆盖：重复收藏无副本、中文多行笔记与 5000 字上限、重启不丢、建议请求不进历史、
//       同一 opId 只记一次、分页（默认 20 / 上限 100）、筛选、暂停、Asia/Shanghai 统计。

#include "dict/dict.h"
#include "vocab/vocab.h"
#include "common/clock.h"
#include "common/stage.h"
#include "sqlite3.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

int gFailures = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "PASS  " : "FAIL  ") << name << "\n";
    if (!ok) ++gFailures;
}

std::string formatDate(std::int64_t utc) {
    std::time_t t = static_cast<std::time_t>(utc);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string shanghaiDate(std::int64_t utc) { return formatDate(utc + 8 * 3600); }

sqlite3* openRaw(const std::string& dir) {
    sqlite3* db = nullptr;
    std::string path = (fs::u8path(dir) / "lexica.db").u8string();
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

EntryId insertEntry(sqlite3* db, const std::string& word) {
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO dictionary_entries(word, phonetic, translation, definition, raw) VALUES(?, '', '释义', 'definition', ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, word.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, word.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : 0;
}

void insertHistory(sqlite3* db, EntryId id, std::int64_t utc, const char* kind) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO vocab_history(entry_id, utc, kind) VALUES(?, ?, ?)", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, utc);
    sqlite3_bind_text(st, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

class FakeStages : public StageSource {
public:
    std::map<EntryId, int> stages;
    int stageOf(EntryId id) const override {
        auto it = stages.find(id);
        return it == stages.end() ? 0 : it->second;
    }
};

const DayCount* findDay(const Stats& stats, const std::string& date) {
    for (const DayCount& day : stats.days) {
        if (day.date == date) return &day;
    }
    return nullptr;
}

std::string freshDir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("lexica_vocab_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.u8string();
}

} // namespace

int main() {
    // ---------- 主流程：业务规则与持久化 ----------
    {
        const std::string dir = freshDir("main");
        Dict dict;
        dict.open(dir);
        sqlite3* raw = openRaw(dir);
        if (!raw) {
            std::cout << "cannot open raw db\n";
            return 2;
        }
        const EntryId apple = insertEntry(raw, "apple");
        const EntryId banana = insertEntry(raw, "banana");
        const EntryId run = insertEntry(raw, "run");
        const EntryId rain = insertEntry(raw, "rain");
        sqlite3_close(raw);
        dict.close();

        Vocab vocab;
        vocab.open(dir);
        FakeStages stages;
        stages.stages[banana] = 2;
        stages.stages[run] = 1;

        // 收藏：重复收藏只有一份
        check(vocab.setFavorite(apple, "apple", true) == ErrorCode::Ok, "setFavorite returns Ok");
        check(vocab.setFavorite(apple, "apple", true) == ErrorCode::Ok, "repeated favorite returns Ok");
        check(vocab.list({1, -1, -1, "", 1, 20}, nullptr).value.total == 1, "repeated favorite keeps a single row");

        // 笔记：中文、多行、引号
        const std::string note = "第一行\n第二行 \"引号\" 'x'\n第三行\t制表";
        check(vocab.setNote(apple, "apple", note) == ErrorCode::Ok, "chinese multi-line note accepted");
        {
            Result<VocabPage> page = vocab.list({1, -1, -1, "", 1, 20}, nullptr);
            check(page.ok() && page.value.items.size() == 1, "favorite list has the word");
            if (page.ok() && page.value.items.size() == 1) {
                check(page.value.items[0].note == note, "note round-trips exactly");
            }
        }

        // 笔记上限 5000 个字符（按 UTF-8 码点计）
        check(vocab.setNote(banana, "banana", std::string(5000, 'a')) == ErrorCode::Ok, "5000-char note accepted");
        check(vocab.setNote(banana, "banana", std::string(5001, 'a')) == ErrorCode::BadRequest, "5001-char note rejected");
        {
            Result<VocabPage> page = vocab.list({-1, -1, -1, "banana", 1, 20}, nullptr);
            check(page.ok() && page.value.items.size() == 1, "banana row exists");
            if (page.ok() && page.value.items.size() == 1) {
                check(page.value.items[0].note == std::string(5000, 'a'), "rejected note leaves the previous content unchanged");
            }
        }
        {
            std::string chinese;
            for (int i = 0; i < 5000; ++i) chinese += "字";
            check(vocab.setNote(rain, "rain", chinese) == ErrorCode::Ok, "5000 chinese chars accepted (UTF-8 counted by codepoint)");
            chinese += "字";
            check(vocab.setNote(rain, "rain", chinese) == ErrorCode::BadRequest, "5001 chinese chars rejected");
        }

        // 暂停
        check(vocab.setPaused(banana, "banana", true) == ErrorCode::Ok, "setPaused returns Ok");
        check(vocab.isPaused(banana), "isPaused true after pause");
        check(vocab.pausedCount() == 1, "pausedCount counts one paused word");
        check(vocab.setPaused(banana, "banana", false) == ErrorCode::Ok, "resume returns Ok");
        check(!vocab.isPaused(banana), "isPaused false after resume");
        check(vocab.pausedCount() == 0, "pausedCount back to zero");
        vocab.setPaused(banana, "banana", true);

        // 查词历史与 opId 幂等
        check(vocab.recordLookup(apple, "apple", "op-1") == ErrorCode::Ok, "recordLookup returns Ok");
        check(vocab.recordLookup(apple, "apple", "op-1") == ErrorCode::Ok, "same opId retry returns Ok");
        check(vocab.history(1, 20).value.total == 1, "same opId does not add a second history row");
        check(vocab.recordLookup(apple, "apple", "") == ErrorCode::BadRequest, "empty opId rejected");
        check(vocab.recordLookup(apple, "apple", "op-2") == ErrorCode::Ok, "different opId records a new row");
        check(vocab.recordLookup(run, "run", "op-3") == ErrorCode::Ok, "recordLookup for another word");
        check(vocab.recordLookup(rain, "rain", "op-4") == ErrorCode::Ok, "recordLookup third word");
        check(vocab.history(1, 20).value.total == 4, "history counts only recorded lookups");

        // 历史分页：最近的在前
        {
            Result<HistoryPage> first = vocab.history(1, 2);
            check(first.ok() && first.value.total == 4 && first.value.items.size() == 2, "history page 1 has 2 of 4");
            if (first.ok() && first.value.items.size() == 2) {
                check(first.value.items[0].entryId == rain, "history is newest first");
            }
            Result<HistoryPage> second = vocab.history(2, 2);
            check(second.ok() && second.value.items.size() == 2, "history page 2 has 2 more rows");
            if (first.ok() && second.ok() && first.value.items.size() == 2 && second.value.items.size() == 2) {
                check(second.value.items[0].entryId == apple, "history page 2 continues where page 1 stopped");
                check(first.value.items[1].entryId != second.value.items[0].entryId, "history pages do not overlap");
            }
        }

        // 筛选
        {
            Result<VocabPage> fav = vocab.list({1, -1, -1, "", 1, 20}, &stages);
            check(fav.ok() && fav.value.total == 1, "favorite filter");
            if (fav.ok() && fav.value.items.size() == 1) check(fav.value.items[0].entryId == apple, "favorite filter picks the right word");
            Result<VocabPage> paused = vocab.list({-1, -1, 1, "", 1, 20}, &stages);
            check(paused.ok() && paused.value.total == 1, "paused filter");
            if (paused.ok() && paused.value.items.size() == 1) check(paused.value.items[0].entryId == banana, "paused filter picks the right word");
            Result<VocabPage> stage2 = vocab.list({-1, 2, -1, "", 1, 20}, &stages);
            check(stage2.ok() && stage2.value.total == 1, "stage filter reads E's stage");
            if (stage2.ok() && stage2.value.items.size() == 1) check(stage2.value.items[0].entryId == banana, "stage filter picks the right word");
            Result<VocabPage> q = vocab.list({-1, -1, -1, "APP", 1, 20}, &stages);
            check(q.ok() && q.value.total == 1, "q filter is case-insensitive");
            if (q.ok() && q.value.items.size() == 1) check(q.value.items[0].entryId == apple, "q filter picks the right word");
            Result<VocabPage> none = vocab.list({-1, -1, -1, "zzz", 1, 20}, &stages);
            check(none.ok() && none.value.total == 0 && none.value.items.empty(), "unmatched q returns an empty page");
        }

        // 分页默认 20、上限 100
        {
            std::vector<std::pair<EntryId, std::string>> extra;
            sqlite3* db = openRaw(dir);
            for (int i = 0; i < 105; ++i) {
                const std::string word = "word-" + std::to_string(i);
                extra.emplace_back(insertEntry(db, word), word);
            }
            sqlite3_close(db);
            for (const auto& item : extra) vocab.setFavorite(item.first, item.second, true);

            Result<VocabPage> def = vocab.list({1, -1, -1, "", 1, 0}, &stages);
            check(def.ok() && def.value.total == 106, "total counts every favorite");
            check(def.ok() && def.value.items.size() == 20, "default page size is 20");
            Result<VocabPage> big = vocab.list({1, -1, -1, "", 1, 500}, &stages);
            check(big.ok() && big.value.items.size() == 100, "page size is capped at 100");
            Result<VocabPage> p2 = vocab.list({1, -1, -1, "", 2, 100}, &stages);
            check(p2.ok() && p2.value.items.size() == 6, "second page holds the remainder");
            if (big.ok() && p2.ok() && !big.value.items.empty() && !p2.value.items.empty()) {
                check(big.value.items[0].entryId < p2.value.items[0].entryId, "pages are ordered and do not overlap");
            }
        }

        // 复习次数与统计
        check(vocab.addReview(apple) == ErrorCode::Ok, "addReview returns Ok");
        check(vocab.addReview(banana) == ErrorCode::Ok, "addReview second word");
        {
            Result<Stats> stats = vocab.stats();
            check(stats.ok() && stats.value.days.size() == 14, "stats returns exactly 14 days");
            if (stats.ok()) {
                const std::string today = shanghaiDate(systemClock().nowUtc());
                const DayCount* day = findDay(stats.value, today);
                check(day != nullptr, "stats contains today's Shanghai date");
                if (day) check(day->lookups == 4 && day->reviews == 2, "today's lookups and reviews counted");
                check(stats.value.days.front().date < stats.value.days.back().date, "days are oldest first");
                int zeros = 0;
                for (const DayCount& d : stats.value.days) {
                    if (d.lookups == 0 && d.reviews == 0) ++zeros;
                }
                check(zeros == 13, "days without activity are filled with zero");
                check(stats.value.paused == 1, "stats reports paused count");
            }
        }

        // 重启后数据仍在
        vocab.close();
        {
            Vocab restarted;
            restarted.open(dir);
            check(restarted.isPaused(banana), "pause state survives restart");
            Result<VocabPage> fav = restarted.list({1, -1, -1, "apple", 1, 20}, nullptr);
            check(fav.ok() && fav.value.total == 1, "favorite survives restart");
            if (fav.ok() && fav.value.items.size() == 1) check(fav.value.items[0].note == note, "note survives restart");
            check(restarted.history(1, 20).value.total == 4, "history survives restart");
            check(restarted.pausedCount() == 1, "pausedCount survives restart");
            check(restarted.recordLookup(apple, "apple", "op-1") == ErrorCode::Ok, "retry opId after restart returns Ok");
            check(restarted.history(1, 20).value.total == 4, "retry opId after restart adds no history");
            Result<Stats> stats = restarted.stats();
            if (stats.ok()) {
                const DayCount* day = findDay(stats.value, shanghaiDate(systemClock().nowUtc()));
                check(day != nullptr && day->lookups == 4 && day->reviews == 2, "stats survive restart");
            } else {
                check(false, "stats survive restart");
            }
        }
    }

    // ---------- Asia/Shanghai 划日期边界 ----------
    {
        const std::string dir = freshDir("tz");
        Dict dict;
        dict.open(dir);
        sqlite3* raw = openRaw(dir);
        const EntryId id = insertEntry(raw, "timezone");
        const std::int64_t now = systemClock().nowUtc();
        // 上海当天 00:00 对应的 UTC 秒
        const std::int64_t boundary = (now + 8 * 3600) / 86400 * 86400 - 8 * 3600;
        // 这两行在 UTC 属于前一天，在上海属于当天
        insertHistory(raw, id, boundary + 3600, "lookup");
        insertHistory(raw, id, boundary, "review");
        sqlite3_close(raw);
        dict.close();

        Vocab vocab;
        vocab.open(dir);
        Result<Stats> stats = vocab.stats();
        const std::string shToday = shanghaiDate(boundary + 3600);
        const std::string utcToday = formatDate(boundary + 3600);
        check(shToday != utcToday, "test data really crosses the Shanghai/UTC date boundary");
        if (stats.ok()) {
            const DayCount* shDay = findDay(stats.value, shToday);
            const DayCount* utcDay = findDay(stats.value, utcToday);
            check(shDay != nullptr && shDay->lookups == 1 && shDay->reviews == 1,
                  "rows just after Shanghai midnight land on the Shanghai date");
            check(utcDay == nullptr || (utcDay->lookups == 0 && utcDay->reviews == 0),
                  "the same rows are not bucketed by the UTC date");
        } else {
            check(false, "stats available for the timezone case");
        }
        vocab.close();
    }

    // ---------- 没有数据库时优雅降级 ----------
    {
        const std::string dir = freshDir("empty");
        Vocab vocab;
        vocab.open(dir);
        check(vocab.setFavorite(1, "x", true) == ErrorCode::DbError, "missing database degrades to DbError");
        check(vocab.recordLookup(1, "x", "op") == ErrorCode::DbError, "recordLookup degrades to DbError");
        check(vocab.setNote(1, "x", "n") == ErrorCode::DbError, "setNote degrades to DbError");
        check(vocab.addReview(1) == ErrorCode::DbError, "addReview degrades to DbError");
        check(!vocab.isPaused(1), "isPaused is false without a database");
        check(vocab.pausedCount() == 0, "pausedCount is zero without a database");
        check(!vocab.list({-1, -1, -1, "", 1, 20}, nullptr).ok(), "list fails without a database");
        check(!vocab.history(1, 20).ok(), "history fails without a database");
        check(!vocab.stats().ok(), "stats fails without a database");
        vocab.close();
    }

    std::cout << (gFailures == 0 ? "ALL VOCAB TESTS PASSED\n" : "VOCAB TEST FAILURES\n");
    return gFailures == 0 ? 0 : 1;
}
