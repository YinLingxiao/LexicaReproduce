#include "vocab/vocab.h"
#include "common/clock.h"
#include "common/text.h"
#include "sqlite3.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <utility>

namespace {

constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kShanghaiOffset = 8 * 3600;

int utf8Chars(const std::string& text) {
    int count = 0;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

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

std::string lowerAscii(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

int clampPage(int page) { return page < 1 ? 1 : page; }

int clampSize(int size) {
    if (size <= 0) return kPageDefault;
    if (size > kPageMax) return kPageMax;
    return size;
}

std::string dbPath(const std::string& dir) {
    if (dir.empty()) return "lexica.db";
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + "lexica.db";
    return dir + "/lexica.db";
}

bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string columnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* value = sqlite3_column_text(stmt, index);
    if (!value) return {};
    int size = sqlite3_column_bytes(stmt, index);
    return std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(size));
}

} // namespace

Vocab::~Vocab() { close(); }

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

void Vocab::close() {
    if (!db_) return;
    sqlite3_close(db_);
    db_ = nullptr;
}

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

ErrorCode Vocab::setFlag(EntryId id, const std::string& word, const char* column, bool value) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
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

ErrorCode Vocab::setFavorite(EntryId id, const std::string& word, bool value) {
    return setFlag(id, word, "favorite", value);
}

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

ErrorCode Vocab::setPaused(EntryId id, const std::string& word, bool value) {
    return setFlag(id, word, "paused", value);
}

Result<VocabPage> Vocab::list(const VocabQuery& query, const StageSource* stages) {
    if (!db_) return Result<VocabPage>::fail(ErrorCode::DbError);
    const std::string needle = lowerAscii(normalizeWord(query.q));

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

    VocabPage page;
    page.total = static_cast<int>(matched.size());
    const int size = clampSize(query.size);
    const int offset = (clampPage(query.page) - 1) * size;
    for (int i = offset; i < page.total && i < offset + size; ++i) {
        page.items.push_back(matched[static_cast<std::size_t>(i)]);
    }
    return Result<VocabPage>::success(page);
}

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

Result<Stats> Vocab::stats() {
    if (!db_) return Result<Stats>::fail(ErrorCode::DbError);
    const std::int64_t now = systemClock().nowUtc();

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
