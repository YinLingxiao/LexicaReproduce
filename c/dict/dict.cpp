#include "dict/dict.h"
#include "common/text.h"
#include "sqlite3.h"
#include <chrono>
#include <functional>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kDatabaseFile = "lexica.db";
constexpr int kDictSchemaVersion = 2;
constexpr int kReviewSchemaVersion = 1;
constexpr int kBatchRows = 5000;

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) stmt_ = nullptr;
    }
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return stmt_; }
    explicit operator bool() const { return stmt_ != nullptr; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

bool execute(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
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

Entry readEntry(sqlite3_stmt* stmt) {
    Entry entry;
    entry.id = sqlite3_column_int64(stmt, 0);
    entry.word = columnText(stmt, 1);
    entry.phonetic = columnText(stmt, 2);
    entry.translation = columnText(stmt, 3);
    entry.definition = columnText(stmt, 4);
    return entry;
}

std::string trim(const std::string& in) {
    std::size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) ++begin;
    std::size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) --end;
    return in.substr(begin, end - begin);
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool integrityOk(sqlite3* db) {
    Statement check(db, "PRAGMA quick_check(1)");
    if (!check || sqlite3_step(check.get()) != SQLITE_ROW) return false;
    return columnText(check.get(), 0) == "ok";
}

int moduleVersion(sqlite3* db, const char* module, bool& ok) {
    Statement query(db, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations WHERE module = ?");
    if (!query || !bindText(query.get(), 1, module) || sqlite3_step(query.get()) != SQLITE_ROW) {
        ok = false;
        return 0;
    }
    ok = true;
    return sqlite3_column_int(query.get(), 0);
}

StartupStatus migrate(sqlite3* db) {
    if (!execute(db, "BEGIN IMMEDIATE")) return StartupStatus::DbCorrupt;
    const char* createMigrations =
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "id INTEGER PRIMARY KEY,"
        "module TEXT NOT NULL,"
        "version INTEGER NOT NULL CHECK(version >= 0),"
        "name TEXT NOT NULL,"
        "applied_utc INTEGER NOT NULL DEFAULT (unixepoch()),"
        "UNIQUE(module, version))";
    if (!execute(db, createMigrations)) {
        execute(db, "ROLLBACK");
        return StartupStatus::DbCorrupt;
    }
    bool ok = false;
    int dictVersion = moduleVersion(db, "dict", ok);
    bool reviewOk = false;
    int reviewVersion = ok ? moduleVersion(db, "review", reviewOk) : 0;
    Statement modules(db, "SELECT DISTINCT module FROM schema_migrations");
    bool foreign = false;
    while (modules && sqlite3_step(modules.get()) == SQLITE_ROW) {
        std::string name = columnText(modules.get(), 0);
        if (name != "dict" && name != "review") foreign = true;
    }
    if (!ok || !reviewOk || !modules) {
        execute(db, "ROLLBACK");
        return StartupStatus::DbCorrupt;
    }
    if (foreign || dictVersion > kDictSchemaVersion || reviewVersion > kReviewSchemaVersion) {
        execute(db, "ROLLBACK");
        return StartupStatus::VersionTooNew;
    }
    if (dictVersion < 1) {
        const char* sql =
            "CREATE TABLE dictionary_entries ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "word TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "phonetic TEXT NOT NULL DEFAULT '',"
            "translation TEXT NOT NULL DEFAULT '',"
            "definition TEXT NOT NULL DEFAULT '');"
            "CREATE INDEX dictionary_entries_word ON dictionary_entries(word COLLATE NOCASE);"
            "CREATE TABLE dictionary_forms ("
            "form TEXT PRIMARY KEY COLLATE NOCASE,"
            "entry_id INTEGER NOT NULL REFERENCES dictionary_entries(id) ON DELETE CASCADE);"
            "CREATE INDEX dictionary_forms_entry_id ON dictionary_forms(entry_id);"
            "INSERT INTO schema_migrations(id, module, version, name) "
            "VALUES(1, 'dict', 1, 'create dictionary tables')";
        if (!execute(db, sql)) {
            execute(db, "ROLLBACK");
            return StartupStatus::DbCorrupt;
        }
    }
    if (dictVersion < 2) {
        const char* sql =
            "ALTER TABLE dictionary_entries ADD COLUMN raw TEXT NOT NULL DEFAULT '';"
            "UPDATE dictionary_entries SET raw = word WHERE raw = '';"
            "INSERT INTO schema_migrations(id, module, version, name) "
            "VALUES(2, 'dict', 2, 'store original spelling')";
        if (!execute(db, sql)) {
            execute(db, "ROLLBACK");
            return StartupStatus::DbCorrupt;
        }
    }
    if (reviewVersion < 1) {
        const char* sql =
            "CREATE TABLE review_state ("
            "entry_id INTEGER PRIMARY KEY,"
            "stage INTEGER NOT NULL,"
            "due_utc INTEGER NOT NULL);"
            "CREATE INDEX review_due ON review_state(due_utc, entry_id);"
            "INSERT INTO schema_migrations(id, module, version, name) "
            "VALUES(3, 'review', 1, 'create review state')";
        if (!execute(db, sql)) {
            execute(db, "ROLLBACK");
            return StartupStatus::DbCorrupt;
        }
    }
    if (!execute(db, "COMMIT")) {
        execute(db, "ROLLBACK");
        return StartupStatus::DbCorrupt;
    }
    return StartupStatus::Ok;
}

struct SourceRow {
    std::string word;
    std::string phonetic;
    std::string definition;
    std::string translation;
    std::string exchange;
};

class Source {
public:
    virtual ~Source() = default;
    virtual int next(SourceRow& row) = 0;
};

class SqliteSource : public Source {
public:
    ~SqliteSource() override {
        if (st_) sqlite3_finalize(st_);
        if (db_) sqlite3_close(db_);
    }
    ErrorCode open(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db_) return ErrorCode::BadRequest;
        Statement columns(db_, "PRAGMA table_info(stardict)");
        if (!columns) return ErrorCode::BadRequest;
        int found = 0;
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(columns.get())) == SQLITE_ROW) {
            std::string name = columnText(columns.get(), 1);
            if (name == "word" || name == "phonetic" || name == "definition" || name == "translation" || name == "exchange") ++found;
        }
        if (rc != SQLITE_DONE || found != 5) return ErrorCode::BadRequest;
        const char* sql = "SELECT ifnull(word,''), ifnull(phonetic,''), ifnull(definition,''), ifnull(translation,''), ifnull(exchange,'') FROM stardict ORDER BY id";
        if (sqlite3_prepare_v2(db_, sql, -1, &st_, nullptr) != SQLITE_OK) return ErrorCode::BadRequest;
        return ErrorCode::Ok;
    }
    int next(SourceRow& row) override {
        int rc = sqlite3_step(st_);
        if (rc == SQLITE_DONE) return 0;
        if (rc != SQLITE_ROW) return -1;
        row.word = columnText(st_, 0);
        row.phonetic = columnText(st_, 1);
        row.definition = columnText(st_, 2);
        row.translation = columnText(st_, 3);
        row.exchange = columnText(st_, 4);
        return 1;
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* st_ = nullptr;
};

class CsvSource : public Source {
public:
    ErrorCode open(const std::string& path) {
        in_.open(fs::u8path(path), std::ios::binary);
        if (!in_) return ErrorCode::BadRequest;
        std::vector<std::string> header;
        if (record(header) != 1) return ErrorCode::BadRequest;
        if (!header.empty() && header[0].compare(0, 3, "\xEF\xBB\xBF") == 0) header[0] = header[0].substr(3);
        for (std::size_t i = 0; i < header.size(); ++i) {
            std::string name = normalizeWord(header[i]);
            int index = static_cast<int>(i);
            if (name == "word") word_ = index;
            else if (name == "phonetic") phonetic_ = index;
            else if (name == "definition") definition_ = index;
            else if (name == "translation") translation_ = index;
            else if (name == "exchange") exchange_ = index;
        }
        if (word_ < 0 || phonetic_ < 0 || definition_ < 0 || translation_ < 0 || exchange_ < 0) return ErrorCode::BadRequest;
        return ErrorCode::Ok;
    }
    int next(SourceRow& row) override {
        std::vector<std::string> fields;
        for (;;) {
            int rc = record(fields);
            if (rc <= 0) return rc;
            if (fields.size() == 1 && fields[0].empty()) continue;
            row.word = field(fields, word_);
            row.phonetic = field(fields, phonetic_);
            row.definition = decode(field(fields, definition_));
            row.translation = decode(field(fields, translation_));
            row.exchange = field(fields, exchange_);
            return 1;
        }
    }

private:
    std::ifstream in_;
    int word_ = -1;
    int phonetic_ = -1;
    int definition_ = -1;
    int translation_ = -1;
    int exchange_ = -1;

    static std::string field(const std::vector<std::string>& fields, int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= fields.size()) return "";
        return fields[static_cast<std::size_t>(index)];
    }
    static std::string decode(const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == '\\')) {
                out.push_back(text[i + 1] == 'n' ? '\n' : '\\');
                ++i;
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    }
    int record(std::vector<std::string>& fields) {
        fields.clear();
        std::string cell;
        bool quoted = false;
        bool any = false;
        char c = 0;
        while (in_.get(c)) {
            any = true;
            if (quoted) {
                if (c != '"') cell.push_back(c);
                else if (in_.peek() == '"') {
                    in_.get(c);
                    cell.push_back('"');
                } else {
                    quoted = false;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                fields.push_back(cell);
                cell.clear();
            } else if (c == '\n') {
                fields.push_back(cell);
                return 1;
            } else if (c != '\r') {
                cell.push_back(c);
            }
        }
        if (in_.bad() || quoted) return -1;
        if (!any) return 0;
        fields.push_back(cell);
        return 1;
    }
};

std::unique_ptr<Source> openSource(const std::string& path, ErrorCode& code) {
    std::ifstream probe(fs::u8path(path), std::ios::binary);
    if (!probe) {
        code = ErrorCode::BadRequest;
        return nullptr;
    }
    char head[16] = {};
    probe.read(head, sizeof(head));
    bool sqlite = probe.gcount() == 16 && std::memcmp(head, "SQLite format 3", 16) == 0;
    probe.close();
    if (sqlite) {
        std::unique_ptr<SqliteSource> src(new SqliteSource());
        code = src->open(path);
        if (code != ErrorCode::Ok) return nullptr;
        return src;
    }
    std::unique_ptr<CsvSource> src(new CsvSource());
    code = src->open(path);
    if (code != ErrorCode::Ok) return nullptr;
    return src;
}

std::vector<std::pair<std::string, std::string>> parseExchange(const std::string& exchange) {
    std::vector<std::pair<std::string, std::string>> values;
    std::size_t begin = 0;
    while (begin <= exchange.size()) {
        std::size_t end = exchange.find('/', begin);
        if (end == std::string::npos) end = exchange.size();
        std::string item = exchange.substr(begin, end - begin);
        std::size_t colon = item.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < item.size()) {
            values.emplace_back(item.substr(0, colon), item.substr(colon + 1));
        }
        if (end == exchange.size()) break;
        begin = end + 1;
    }
    return values;
}

std::vector<std::string> splitForms(const std::string& value) {
    std::vector<std::string> forms;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        std::size_t end = value.find(',', begin);
        if (end == std::string::npos) end = value.size();
        std::string form = normalizeWord(value.substr(begin, end - begin));
        if (!form.empty()) forms.push_back(form);
        if (end == value.size()) break;
        begin = end + 1;
    }
    return forms;
}

bool addForm(sqlite3_stmt* findEntry, sqlite3_stmt* insertForm, const std::string& rawForm, const std::string& rawLemma) {
    std::string form = normalizeWord(rawForm);
    std::string lemma = normalizeWord(rawLemma);
    if (form.empty() || lemma.empty() || form == lemma) return true;
    sqlite3_reset(findEntry);
    sqlite3_clear_bindings(findEntry);
    if (!bindText(findEntry, 1, lemma)) return false;
    int rc = sqlite3_step(findEntry);
    if (rc == SQLITE_DONE) return true;
    if (rc != SQLITE_ROW) return false;
    sqlite3_reset(insertForm);
    sqlite3_clear_bindings(insertForm);
    if (!bindText(insertForm, 1, form) || sqlite3_bind_int64(insertForm, 2, sqlite3_column_int64(findEntry, 0)) != SQLITE_OK) return false;
    return sqlite3_step(insertForm) == SQLITE_DONE;
}

bool commitBatch(sqlite3* db, bool& open) {
    if (!open) return true;
    if (!execute(db, "COMMIT")) return false;
    open = false;
    return true;
}

ErrorCode importRows(const std::string& dbFile, const std::string& srcPath, const std::atomic<bool>& stop, ImportReport& report,
                      const std::function<void(const ImportReport&)>& progress) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbFile.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return ErrorCode::DbError;
    }
    sqlite3_busy_timeout(db, 5000);
    execute(db, "PRAGMA foreign_keys = ON");
    ErrorCode code = ErrorCode::Ok;
    for (int pass = 0; pass < 2 && code == ErrorCode::Ok; ++pass) {
        ErrorCode srcCode = ErrorCode::Ok;
        std::unique_ptr<Source> src = openSource(srcPath, srcCode);
        if (!src) {
            code = srcCode == ErrorCode::BadRequest && pass == 1 ? ErrorCode::DbError : srcCode;
            break;
        }
        Statement find(db, "SELECT id, raw FROM dictionary_entries WHERE word = ? LIMIT 1");
        Statement insert(db, "INSERT INTO dictionary_entries(word, raw, phonetic, translation, definition) VALUES(?,?,?,?,?)");
        Statement update(db, "UPDATE dictionary_entries SET raw=?1, phonetic=?2, translation=?3, definition=?4 "
                             "WHERE id=?5 AND (raw IS NOT ?1 OR phonetic IS NOT ?2 OR translation IS NOT ?3 OR definition IS NOT ?4)");
        Statement findLemma(db, "SELECT id FROM dictionary_entries WHERE word = ? LIMIT 1");
        Statement insertForm(db, "INSERT INTO dictionary_forms(form, entry_id) VALUES(?, ?) ON CONFLICT(form) DO UPDATE SET entry_id = excluded.entry_id");
        if ((pass == 0 && (!find || !insert || !update)) || (pass == 1 && (!findLemma || !insertForm))) {
            code = ErrorCode::DbError;
            break;
        }
        bool tx = false;
        int rows = 0;
        ImportReport committed = report;
        SourceRow row;
        while (code == ErrorCode::Ok) {
            if (stop) {
                code = ErrorCode::Internal;
                break;
            }
            int next = src->next(row);
            if (next == 0) break;
            if (next < 0) {
                code = ErrorCode::BadRequest;
                break;
            }
            if (!tx && !execute(db, "BEGIN IMMEDIATE")) {
                code = ErrorCode::DbError;
                break;
            }
            tx = true;
            if (pass == 0) {
                std::string raw = trim(row.word);
                std::string word = normalizeWord(raw);
                if (word.empty()) {
                    ++report.failed;
                } else {
                    sqlite3_reset(find.get());
                    bindText(find.get(), 1, word);
                    int step = sqlite3_step(find.get());
                    if (step == SQLITE_ROW) {
                        bool oldLower = columnText(find.get(), 1).empty() || columnText(find.get(), 1) == word;
                        if (oldLower && raw != word) {
                            ++report.skipped;
                        } else {
                            sqlite3_reset(update.get());
                            bindText(update.get(), 1, raw);
                            bindText(update.get(), 2, row.phonetic);
                            bindText(update.get(), 3, row.translation);
                            bindText(update.get(), 4, row.definition);
                            sqlite3_bind_int64(update.get(), 5, sqlite3_column_int64(find.get(), 0));
                            if (sqlite3_step(update.get()) != SQLITE_DONE) code = ErrorCode::DbError;
                            else if (sqlite3_changes(db) > 0) ++report.updated;
                            else ++report.skipped;
                        }
                    } else if (step == SQLITE_DONE) {
                        sqlite3_reset(insert.get());
                        bindText(insert.get(), 1, word);
                        bindText(insert.get(), 2, raw);
                        bindText(insert.get(), 3, row.phonetic);
                        bindText(insert.get(), 4, row.translation);
                        bindText(insert.get(), 5, row.definition);
                        if (sqlite3_step(insert.get()) != SQLITE_DONE) code = ErrorCode::DbError;
                        else ++report.added;
                    } else {
                        code = ErrorCode::DbError;
                    }
                }
            } else {
                std::string word = normalizeWord(row.word);
                if (!word.empty()) {
                    for (const auto& value : parseExchange(row.exchange)) {
                        if (value.first == "0") {
                            if (!addForm(findLemma.get(), insertForm.get(), word, value.second)) code = ErrorCode::DbError;
                        } else if (value.first == "p" || value.first == "d" || value.first == "i" || value.first == "3" ||
                                   value.first == "s" || value.first == "r" || value.first == "t") {
                            for (const std::string& form : splitForms(value.second)) {
                                if (!addForm(findLemma.get(), insertForm.get(), form, word)) code = ErrorCode::DbError;
                            }
                        }
                    }
                }
            }
            if (code == ErrorCode::Ok && ++rows >= kBatchRows) {
                if (!commitBatch(db, tx)) {
                    code = ErrorCode::DbError;
                } else {
                    committed = report;
                    if (progress) progress(report);
                }
                rows = 0;
            }
        }
        if (code == ErrorCode::Ok && tx && !commitBatch(db, tx)) code = ErrorCode::DbError;
        if (tx) {
            execute(db, "ROLLBACK");
            report = committed;
        }
    }
    sqlite3_close(db);
    if (progress) progress(report);
    return code;
}

}

const char* startupName(StartupStatus status) {
    switch (status) {
    case StartupStatus::Ok: return "OK";
    case StartupStatus::DictEmpty: return "DICT_EMPTY";
    case StartupStatus::DirNotWritable: return "DIR_NOT_WRITABLE";
    case StartupStatus::DbCorrupt: return "DB_CORRUPT";
    case StartupStatus::VersionTooNew: return "VERSION_TOO_NEW";
    }
    return "INTERNAL";
}

Dict::~Dict() { close(); }

void Dict::close() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    stop_ = false;
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    ready_ = false;
}

bool Dict::ready() const { return ready_; }

bool Dict::refreshReady() {
    if (!db_) return false;
    Statement count(db_, "SELECT EXISTS(SELECT 1 FROM dictionary_entries LIMIT 1)");
    if (!count || sqlite3_step(count.get()) != SQLITE_ROW) return false;
    ready_ = sqlite3_column_int(count.get(), 0) != 0;
    return true;
}

StartupStatus Dict::open(const std::string& dataDir) {
    close();
    dataDir_ = dataDir;
    dbPath_ = (fs::u8path(dataDir_) / kDatabaseFile).u8string();
    seq_ = nowMs();
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dbPath_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return rc == SQLITE_CANTOPEN ? StartupStatus::DirNotWritable : StartupStatus::DbCorrupt;
    }
    sqlite3_busy_timeout(db, 5000);
    if (!integrityOk(db)) {
        sqlite3_close(db);
        return StartupStatus::DbCorrupt;
    }
    StartupStatus status = migrate(db);
    if (status != StartupStatus::Ok || !execute(db, "PRAGMA foreign_keys = ON") || !execute(db, "PRAGMA journal_mode=WAL")) {
        sqlite3_close(db);
        return status == StartupStatus::VersionTooNew ? status : StartupStatus::DbCorrupt;
    }
    db_ = db;
    if (!refreshReady()) {
        close();
        return StartupStatus::DbCorrupt;
    }
    return ready_ ? StartupStatus::Ok : StartupStatus::DictEmpty;
}

Result<Entry> Dict::lookup(const std::string& word) {
    std::string key = normalizeWord(word);
    if (key.empty()) return Result<Entry>::fail(ErrorCode::BadRequest);
    if (!ready_) return Result<Entry>::fail(ErrorCode::DictNotReady);
    Statement exact(db_, "SELECT id, word, phonetic, translation, definition FROM dictionary_entries WHERE word = ? LIMIT 1");
    if (!exact || !bindText(exact.get(), 1, key)) return Result<Entry>::fail(ErrorCode::DbError);
    int rc = sqlite3_step(exact.get());
    if (rc == SQLITE_ROW) {
        Entry entry = readEntry(exact.get());
        entry.query = key;
        return Result<Entry>::success(entry);
    }
    if (rc != SQLITE_DONE) return Result<Entry>::fail(ErrorCode::DbError);
    Statement form(db_, "SELECT e.id, e.word, e.phonetic, e.translation, e.definition FROM dictionary_forms f "
                        "JOIN dictionary_entries e ON e.id = f.entry_id WHERE f.form = ? LIMIT 1");
    if (!form || !bindText(form.get(), 1, key)) return Result<Entry>::fail(ErrorCode::DbError);
    rc = sqlite3_step(form.get());
    if (rc == SQLITE_ROW) {
        Entry entry = readEntry(form.get());
        entry.query = key;
        return Result<Entry>::success(entry);
    }
    if (rc != SQLITE_DONE) return Result<Entry>::fail(ErrorCode::DbError);
    return Result<Entry>::fail(ErrorCode::WordNotFound);
}

Result<std::vector<SuggestItem>> Dict::suggest(const std::string& prefix) {
    std::vector<SuggestItem> items;
    std::string key = normalizeWord(prefix);
    if (key.empty()) return Result<std::vector<SuggestItem>>::success(items);
    if (!ready_) return Result<std::vector<SuggestItem>>::fail(ErrorCode::DictNotReady);
    Statement query(db_, "SELECT id, word FROM dictionary_entries WHERE word >= ? AND word < ? ORDER BY word, id LIMIT ?");
    if (!query || !bindText(query.get(), 1, key) || !bindText(query.get(), 2, key + "\xFF") ||
        sqlite3_bind_int(query.get(), 3, kSuggestMax) != SQLITE_OK) {
        return Result<std::vector<SuggestItem>>::fail(ErrorCode::DbError);
    }
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(query.get())) == SQLITE_ROW) {
        SuggestItem item;
        item.id = sqlite3_column_int64(query.get(), 0);
        item.word = columnText(query.get(), 1);
        items.push_back(item);
    }
    if (rc != SQLITE_DONE) return Result<std::vector<SuggestItem>>::fail(ErrorCode::DbError);
    return Result<std::vector<SuggestItem>>::success(items);
}

Result<Entry> Dict::get(EntryId id) {
    if (!ready_) return Result<Entry>::fail(ErrorCode::DictNotReady);
    Statement query(db_, "SELECT id, word, phonetic, translation, definition FROM dictionary_entries WHERE id = ? LIMIT 1");
    if (!query || sqlite3_bind_int64(query.get(), 1, id) != SQLITE_OK) return Result<Entry>::fail(ErrorCode::DbError);
    int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return Result<Entry>::success(readEntry(query.get()));
    if (rc == SQLITE_DONE) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::fail(ErrorCode::DbError);
}

std::vector<std::string> Dict::forms(const std::string& word) {
    std::vector<std::string> out;
    std::string key = normalizeWord(word);
    if (!db_ || key.empty()) return out;
    Statement query(db_, "SELECT f.form FROM dictionary_forms f JOIN dictionary_entries e ON e.id = f.entry_id "
                         "WHERE e.word = ? ORDER BY length(f.form) DESC, f.form");
    if (!query || !bindText(query.get(), 1, key)) return out;
    while (sqlite3_step(query.get()) == SQLITE_ROW) out.push_back(columnText(query.get(), 0));
    return out;
}

Result<ImportReport> Dict::importFile(const std::string& path) {
    if (path.empty()) return Result<ImportReport>::fail(ErrorCode::BadRequest);
    if (!db_) return Result<ImportReport>::fail(ErrorCode::DbError);
    ErrorCode code = ErrorCode::Ok;
    if (!openSource(path, code)) return Result<ImportReport>::fail(code);
    std::atomic<bool> stop{false};
    ImportReport report;
    code = importRows(dbPath_, path, stop, report, nullptr);
    if (code != ErrorCode::Ok || !refreshReady()) return Result<ImportReport>::fail(code == ErrorCode::Ok ? ErrorCode::DbError : code);
    return Result<ImportReport>::success(report);
}

Result<ImportTask> Dict::importStart(const std::string& path) {
    if (path.empty()) return Result<ImportTask>::fail(ErrorCode::BadRequest);
    if (!db_) return Result<ImportTask>::fail(ErrorCode::DbError);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (task_.status == "running") return Result<ImportTask>::success(task_);
    }
    ErrorCode code = ErrorCode::Ok;
    if (!openSource(path, code)) return Result<ImportTask>::fail(code);
    if (worker_.joinable()) worker_.join();
    ImportTask task;
    task.id = "i" + std::to_string(++seq_);
    task.status = "running";
    {
        std::lock_guard<std::mutex> lock(mu_);
        task_ = task;
    }
    std::string file = dbPath_;
    worker_ = std::thread([this, file, path] {
        ImportReport report;
        ErrorCode result = importRows(file, path, stop_, report, [this](const ImportReport& part) {
            std::lock_guard<std::mutex> lock(mu_);
            task_.report = part;
        });
        refreshReady();
        std::lock_guard<std::mutex> lock(mu_);
        task_.report = report;
        task_.status = result == ErrorCode::Ok ? "succeeded" : "failed";
        task_.error = result;
    });
    return Result<ImportTask>::success(task);
}

Result<ImportTask> Dict::importTask(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (id.empty() || id != task_.id) return Result<ImportTask>::fail(ErrorCode::BadRequest);
    return Result<ImportTask>::success(task_);
}
