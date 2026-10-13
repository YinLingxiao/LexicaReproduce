// 个人词汇模块：保存收藏、笔记、暂停和活动记录，提供列表与统计。
#include "vocab/vocab.h"
#include "common/clock.h"
#include "common/text.h"
#include "sqlite3.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <utility>

namespace {

constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kShanghaiOffset = 8 * 3600;

// 数 UTF-8 字符的起始字节，使中文笔记按字符数而非字节数限制。
int utf8Chars(const std::string& text) {
    int count = 0;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

// 把时间戳格式化为 YYYY-MM-DD 日期字符串。
std::string formatDate(std::int64_t utc) {
    std::time_t t = static_cast<std::time_t>(utc);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// Statistics are bucketed by Asia/Shanghai (UTC+8, no daylight saving).
std::string shanghaiDate(std::int64_t utc) { return formatDate(utc + kShanghaiOffset); }

// 把文本转成小写，供不区分大小写的搜索使用。
std::string lowerAscii(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

// 页码小于 1 时回到第 1 页。
int clampPage(int page) { return page < 1 ? 1 : page; }

// 修正每页数量：默认 20 条，最多 100 条。
int clampSize(int size) {
    if (size <= 0) return kPageDefault;
    if (size > kPageMax) return kPageMax;
    return size;
}

// 拼出 dataDir 下共享数据库 lexica.db 的路径。
std::string dbPath(const std::string& dir) {
    if (dir.empty()) return "lexica.db";
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + "lexica.db";
    return dir + "/lexica.db";
}

// 执行不需要逐行取结果的 SQL，并返回是否成功。
bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

// 把字符串绑定到 SQL 的问号参数，避免手工拼接用户输入。
bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

// 读取查询结果中的文本，按实际字节数取值；NULL 当作空串。
std::string columnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* value = sqlite3_column_text(stmt, index);
    if (!value) return {};
    int size = sqlite3_column_bytes(stmt, index);
    return std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(size));
}

} // namespace

Vocab::~Vocab() { close(); }

// 打开词库模块已建好的共享库并检查个人词汇表；失败时保持连接为空。
void Vocab::open(const std::string& dataDir) {
    close();
    dataDir_ = dataDir;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath(dataDir).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='vocab_item'", -1, &stmt, nullptr) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW;
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) {
        sqlite3_close(db);
        return;
    }
    db_ = db;
}

// 关闭数据库连接，释放本模块持有的资源。
void Vocab::close() {
    if (!db_) return;
    sqlite3_close(db_);
    db_ = nullptr;
}

// 确保词条在个人词汇表中有一行，并同步其词头。
bool Vocab::ensureItem(EntryId id, const std::string& word) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO vocab_item(entry_id, word) VALUES(?, ?)", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    bindText(stmt, 2, word);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    if (word.empty()) return true;

    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE vocab_item SET word=? WHERE entry_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bindText(stmt, 1, word);
    sqlite3_bind_int64(stmt, 2, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// 收藏和暂停共用的写入过程：先建记录，再在事务内更新开关。
ErrorCode Vocab::setFlag(EntryId id, const std::string& word, const char* column, bool value) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    // 先取得写事务；建词汇记录与修改开关要么一起成功，要么一起回滚。
    if (!exec(db_, "BEGIN IMMEDIATE")) return ErrorCode::DbError;
    if (!ensureItem(id, word)) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    const std::string sql = std::string("UPDATE vocab_item SET ") + column + "=? WHERE entry_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    sqlite3_bind_int(stmt, 1, value ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || !exec(db_, "COMMIT")) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    return ErrorCode::Ok;
}

// 用 opId 去重；首次请求同时写个人词汇记录和查词事件。
ErrorCode Vocab::recordLookup(EntryId id, const std::string& word, const OpId& opId) {
    if (opId.empty() || id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    if (!exec(db_, "BEGIN IMMEDIATE")) return ErrorCode::DbError;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO vocab_lookup_op(op_id, entry_id, utc) VALUES(?, ?, ?)", -1, &stmt, nullptr) != SQLITE_OK) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    bindText(stmt, 1, opId);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_bind_int64(stmt, 3, systemClock().nowUtc());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    // INSERT OR IGNORE 没新增行，说明这个 opId 已处理过，直接返回避免重复记历史。
    if (sqlite3_changes(db_) == 0) {
        // The same op_id was already recorded: a retry must not add history again.
        exec(db_, "COMMIT");
        return ErrorCode::Ok;
    }
    if (!ensureItem(id, word)) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO vocab_history(entry_id, utc, kind) VALUES(?, ?, 'lookup')", -1, &stmt, nullptr) != SQLITE_OK) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, systemClock().nowUtc());
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || !exec(db_, "COMMIT")) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    return ErrorCode::Ok;
}

// 设置或取消收藏；同一个词共用一条个人词汇记录。
ErrorCode Vocab::setFavorite(EntryId id, const std::string& word, bool value) {
    return setFlag(id, word, "favorite", value);
}

// 保存笔记；超过 5000 个字符时拒绝，不覆盖原笔记。
ErrorCode Vocab::setNote(EntryId id, const std::string& word, const std::string& text) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    if (utf8Chars(text) > kNoteMaxChars) return ErrorCode::BadRequest;
    if (!exec(db_, "BEGIN IMMEDIATE")) return ErrorCode::DbError;
    if (!ensureItem(id, word)) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE vocab_item SET note=? WHERE entry_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    bindText(stmt, 1, text);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || !exec(db_, "COMMIT")) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    return ErrorCode::Ok;
}

// 设置是否暂停复习；暂停后选题时会跳过这个词。
ErrorCode Vocab::setPaused(EntryId id, const std::string& word, bool value) {
    return setFlag(id, word, "paused", value);
}

// 筛选收藏、暂停、词头和学习阶段，再返回当前页及筛选后的总数。
Result<VocabPage> Vocab::list(const VocabQuery& query, const StageSource* stages) {
    if (!db_) return Result<VocabPage>::fail(ErrorCode::DbError);
    const std::string needle = lowerAscii(normalizeWord(query.q));

    // SQL 先筛收藏和暂停，再逐行筛词头和复习模块提供的学习阶段。
    std::vector<VocabRow> matched;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT entry_id, word, favorite, note, paused FROM vocab_item "
        "WHERE (? < 0 OR favorite = ?) AND (? < 0 OR paused = ?) ORDER BY entry_id";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<VocabPage>::fail(ErrorCode::DbError);
    }
    sqlite3_bind_int(stmt, 1, query.favorite);
    sqlite3_bind_int(stmt, 2, query.favorite);
    sqlite3_bind_int(stmt, 3, query.paused);
    sqlite3_bind_int(stmt, 4, query.paused);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        VocabRow row;
        row.entryId = sqlite3_column_int64(stmt, 0);
        row.word = columnText(stmt, 1);
        row.favorite = sqlite3_column_int(stmt, 2) != 0;
        row.note = columnText(stmt, 3);
        row.paused = sqlite3_column_int(stmt, 4) != 0;
        if (!needle.empty() && lowerAscii(row.word).find(needle) == std::string::npos) continue;
        row.stage = stages ? stages->stageOf(row.entryId) : 0;
        if (query.stage >= 0 && row.stage != query.stage) continue;
        matched.push_back(row);
    }
    sqlite3_finalize(stmt);

    // 先得到筛选后总数，再截取当前页，确保列表和总数使用相同条件。
    VocabPage page;
    if (matched.size() > static_cast<std::size_t>(0x7fffffff)) return Result<VocabPage>::fail(ErrorCode::BadRequest);
    page.total = static_cast<int>(matched.size());
    const int size = clampSize(query.size);
    const std::int64_t pageNo = query.page < 1 ? 1 : static_cast<std::int64_t>(query.page);
    const std::int64_t offset = (pageNo - 1) * static_cast<std::int64_t>(size);
    if (offset < 0 || offset >= page.total) return Result<VocabPage>::success(page);
    const std::int64_t end = offset + size < page.total ? offset + size : page.total;
    for (std::int64_t i = offset; i < end; ++i) page.items.push_back(matched[static_cast<std::size_t>(i)]);
    return Result<VocabPage>::success(page);
}

// 读取正式查词历史，按最近发生的记录优先排列并分页。
Result<HistoryPage> Vocab::history(int pageNo, int sizeIn) {
    if (!db_) return Result<HistoryPage>::fail(ErrorCode::DbError);
    HistoryPage page;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM vocab_history WHERE kind='lookup'", -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<HistoryPage>::fail(ErrorCode::DbError);
    }
    page.total = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);

    const int size = clampSize(sizeIn);
    sqlite3_int64 offset = static_cast<sqlite3_int64>(clampPage(pageNo) - 1) * size;
    if (offset > page.total) offset = page.total;

    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT entry_id, utc FROM vocab_history WHERE kind='lookup' ORDER BY seq DESC LIMIT ? OFFSET ?", -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<HistoryPage>::fail(ErrorCode::DbError);
    }
    sqlite3_bind_int(stmt, 1, size);
    sqlite3_bind_int64(stmt, 2, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryItem item;
        item.entryId = sqlite3_column_int64(stmt, 0);
        item.utc = sqlite3_column_int64(stmt, 1);
        page.items.push_back(item);
    }
    sqlite3_finalize(stmt);
    return Result<HistoryPage>::success(page);
}

// 按上海日期汇总查词和复习事件，输出含今天的 14 天，空白日期补 0。
Result<Stats> Vocab::stats() {
    if (!db_) return Result<Stats>::fail(ErrorCode::DbError);
    const std::int64_t now = systemClock().nowUtc();

    // 按“上海日期→查词数/复习数”汇总，统一用 UTC+8 划分一天。
    std::map<std::string, std::pair<int, int>> counts;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT date(utc + 28800, 'unixepoch'), kind, COUNT(*) FROM vocab_history "
        "WHERE utc >= ? GROUP BY 1, 2";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<Stats>::fail(ErrorCode::DbError);
    }
    sqlite3_bind_int64(stmt, 1, now - 14 * kDay);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string date = columnText(stmt, 0);
        if (date.empty()) continue;
        const std::string kind = columnText(stmt, 1);
        const int count = sqlite3_column_int(stmt, 2);
        if (kind == "lookup") counts[date].first += count;
        else if (kind == "review") counts[date].second += count;
    }
    sqlite3_finalize(stmt);

    Stats stats;
    // 从 13 天前排到今天；当天没有事件时次数保持为 0。
    for (int i = 13; i >= 0; --i) {
        DayCount day;
        day.date = shanghaiDate(now - static_cast<std::int64_t>(i) * kDay);
        const auto found = counts.find(day.date);
        if (found != counts.end()) {
            day.lookups = found->second.first;
            day.reviews = found->second.second;
        }
        stats.days.push_back(day);
    }
    stats.paused = pausedCount();
    return Result<Stats>::success(stats);
}

// 在调用方已经打开的事务里写入正式复习事件；题目编号相同则不能插第二条。
bool insertReviewHistory(sqlite3* db, EntryId id, QuestionId qid, std::int64_t utc) {
    if (!db || id <= 0 || qid <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO vocab_history(entry_id, utc, kind, qid) VALUES(?, ?, 'review', ?)", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, utc);
    sqlite3_bind_int64(stmt, 3, qid);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// 追加一次正式复习事件，供最近 14 天活动统计使用。
ErrorCode Vocab::addReview(EntryId id) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO vocab_history(entry_id, utc, kind) VALUES(?, ?, 'review')", -1, &stmt, nullptr) != SQLITE_OK) {
        return ErrorCode::DbError;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, systemClock().nowUtc());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? ErrorCode::Ok : ErrorCode::DbError;
}

// 按题目补记正式复习；同一题已有记录时直接成功，不增加次数。
ErrorCode Vocab::addReview(EntryId id, QuestionId qid) {
    if (id <= 0 || qid <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    if (!exec(db_, "BEGIN IMMEDIATE")) return ErrorCode::DbError;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM vocab_history WHERE kind='review' AND qid=?", -1, &stmt, nullptr) != SQLITE_OK) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    sqlite3_bind_int64(stmt, 1, qid);
    const int found = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (found == SQLITE_ROW) {
        if (!exec(db_, "COMMIT")) {
            exec(db_, "ROLLBACK");
            return ErrorCode::DbError;
        }
        return ErrorCode::Ok;
    }
    if (found != SQLITE_DONE) {
        exec(db_, "ROLLBACK");
        return ErrorCode::DbError;
    }
    if (!insertReviewHistory(db_, id, qid, systemClock().nowUtc()) || !exec(db_, "COMMIT")) {
        exec(db_, "ROLLBACK");
        stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM vocab_history WHERE kind='review' AND qid=?", -1, &stmt, nullptr) != SQLITE_OK) return ErrorCode::DbError;
        sqlite3_bind_int64(stmt, 1, qid);
        const int again = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return again == SQLITE_ROW ? ErrorCode::Ok : ErrorCode::DbError;
    }
    return ErrorCode::Ok;
}

// 读取这个词是否暂停；复习模块选题时通过它过滤。
bool Vocab::isPaused(EntryId id) const {
    if (!db_ || id <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT paused FROM vocab_item WHERE entry_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    const bool paused = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return paused;
}

// 统计暂停复习的词条数量。
int Vocab::pausedCount() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM vocab_item WHERE paused=1", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}
