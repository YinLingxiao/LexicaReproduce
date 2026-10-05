#include "dict/dict.h"
#include "common/text.h"
#include "sqlite3.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kDatabaseFile = "lexica.db";
constexpr int kDictSchemaVersion = 1;

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

class DatabaseHandle {
public:
    ~DatabaseHandle() {
        if (db) sqlite3_close(db);
    }

    sqlite3* release() {
        sqlite3* value = db;
        db = nullptr;
        return value;
    }

    sqlite3* db = nullptr;
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

bool integrityOk(sqlite3* db) {
    Statement check(db, "PRAGMA quick_check(1)");
    if (!check || sqlite3_step(check.get()) != SQLITE_ROW) return false;
    return columnText(check.get(), 0) == "ok";
}

int dictSchemaVersion(sqlite3* db, bool& ok) {
    Statement query(db,
        "SELECT COALESCE(MAX(version), 0) "
        "FROM schema_migrations WHERE module = 'dict'");
    if (!query || sqlite3_step(query.get()) != SQLITE_ROW) {
        ok = false;
        return 0;
    }
    ok = true;
    return sqlite3_column_int(query.get(), 0);
}

StartupStatus migrate(sqlite3* db) {
    if (!execute(db, "BEGIN IMMEDIATE")) return StartupStatus::DbCorrupt;

    const char* createMigrationTable =
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "id INTEGER PRIMARY KEY,"
        "module TEXT NOT NULL,"
        "version INTEGER NOT NULL CHECK(version >= 0),"
        "name TEXT NOT NULL,"
        "applied_utc INTEGER NOT NULL DEFAULT (unixepoch()),"
        "UNIQUE(module, version)"
        ")";
    if (!execute(db, createMigrationTable)) {
        execute(db, "ROLLBACK");
        return StartupStatus::DbCorrupt;
    }

    bool versionOk = false;
    int version = dictSchemaVersion(db, versionOk);
    if (!versionOk) {
        execute(db, "ROLLBACK");
        return StartupStatus::DbCorrupt;
    }
    if (version > kDictSchemaVersion) {
        execute(db, "ROLLBACK");
        return StartupStatus::VersionTooNew;
    }

    if (version < 1) {
        const char* migration =
            "CREATE TABLE dictionary_entries ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "word TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "phonetic TEXT NOT NULL DEFAULT '',"
            "translation TEXT NOT NULL DEFAULT '',"
            "definition TEXT NOT NULL DEFAULT ''"
            ");"
            "CREATE INDEX dictionary_entries_word "
            "ON dictionary_entries(word COLLATE NOCASE);"
            "CREATE TABLE dictionary_forms ("
            "form TEXT PRIMARY KEY COLLATE NOCASE,"
            "entry_id INTEGER NOT NULL REFERENCES dictionary_entries(id) ON DELETE CASCADE"
            ");"
            "CREATE INDEX dictionary_forms_entry_id ON dictionary_forms(entry_id);"
            "INSERT INTO schema_migrations(id, module, version, name) "
            "VALUES(1, 'dict', 1, 'create dictionary tables')";
        if (!execute(db, migration)) {
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

bool hasEcdictSchema(sqlite3* db) {
    Statement columns(db, "PRAGMA table_info(stardict)");
    if (!columns) return false;

    bool id = false;
    bool word = false;
    bool phonetic = false;
    bool definition = false;
    bool translation = false;
    bool exchange = false;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(columns.get())) == SQLITE_ROW) {
        std::string name = columnText(columns.get(), 1);
        if (name == "id") id = true;
        else if (name == "word") word = true;
        else if (name == "phonetic") phonetic = true;
        else if (name == "definition") definition = true;
        else if (name == "translation") translation = true;
        else if (name == "exchange") exchange = true;
    }
    return rc == SQLITE_DONE && id && word && phonetic && definition && translation && exchange;
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

bool addForm(sqlite3_stmt* findEntry, sqlite3_stmt* insertForm,
             const std::string& rawForm, const std::string& rawLemma) {
    std::string form = normalizeWord(rawForm);
    std::string lemma = normalizeWord(rawLemma);
    if (form.empty() || lemma.empty() || form == lemma) return true;

    sqlite3_reset(findEntry);
    sqlite3_clear_bindings(findEntry);
    if (!bindText(findEntry, 1, lemma)) return false;
    int rc = sqlite3_step(findEntry);
    if (rc == SQLITE_DONE) return true;
    if (rc != SQLITE_ROW) return false;
    EntryId entryId = sqlite3_column_int64(findEntry, 0);

    sqlite3_reset(insertForm);
    sqlite3_clear_bindings(insertForm);
    if (!bindText(insertForm, 1, form) || sqlite3_bind_int64(insertForm, 2, entryId) != SQLITE_OK) {
        return false;
    }
    return sqlite3_step(insertForm) == SQLITE_DONE;
}

std::string escapeLike(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '%' || c == '_') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

} // namespace

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

StartupStatus Dict::open(const std::string& dataDir) {
    close();
    dataDir_ = dataDir;
    dbPath_ = (fs::u8path(dataDir_) / kDatabaseFile).u8string();

    DatabaseHandle opened;
    int rc = sqlite3_open_v2(dbPath_.c_str(), &opened.db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK || !opened.db) return StartupStatus::DbCorrupt;

    sqlite3_busy_timeout(opened.db, 5000);
    if (!execute(opened.db, "PRAGMA foreign_keys = ON") || !integrityOk(opened.db)) {
        return StartupStatus::DbCorrupt;
    }

    StartupStatus status = migrate(opened.db);
    if (status != StartupStatus::Ok) return status;

    db_ = opened.release();
    if (!refreshReady()) {
        close();
        return StartupStatus::DbCorrupt;
    }
    return ready_ ? StartupStatus::Ok : StartupStatus::DictEmpty;
}

void Dict::close() {
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

Result<Entry> Dict::findWord(const std::string& word) const {
    Statement query(db_,
        "SELECT id, word, phonetic, translation, definition "
        "FROM dictionary_entries WHERE word = ? COLLATE NOCASE LIMIT 1");
    if (!query || !bindText(query.get(), 1, word)) return Result<Entry>::fail(ErrorCode::DbError);
    int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return Result<Entry>::success(readEntry(query.get()));
    if (rc == SQLITE_DONE) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::fail(ErrorCode::DbError);
}

Result<Entry> Dict::findForm(const std::string& form) const {
    Statement query(db_,
        "SELECT e.id, e.word, e.phonetic, e.translation, e.definition "
        "FROM dictionary_forms f "
        "JOIN dictionary_entries e ON e.id = f.entry_id "
        "WHERE f.form = ? COLLATE NOCASE LIMIT 1");
    if (!query || !bindText(query.get(), 1, form)) return Result<Entry>::fail(ErrorCode::DbError);
    int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return Result<Entry>::success(readEntry(query.get()));
    if (rc == SQLITE_DONE) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::fail(ErrorCode::DbError);
}

Result<Entry> Dict::findId(EntryId id) const {
    Statement query(db_,
        "SELECT id, word, phonetic, translation, definition "
        "FROM dictionary_entries WHERE id = ? LIMIT 1");
    if (!query || sqlite3_bind_int64(query.get(), 1, id) != SQLITE_OK) {
        return Result<Entry>::fail(ErrorCode::DbError);
    }
    int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return Result<Entry>::success(readEntry(query.get()));
    if (rc == SQLITE_DONE) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::fail(ErrorCode::DbError);
}

Result<Entry> Dict::lookup(const std::string& word) {
    std::string key = normalizeWord(word);
    if (key.empty()) return Result<Entry>::fail(ErrorCode::BadRequest);
    if (!ready_) return Result<Entry>::fail(ErrorCode::DictNotReady);
    Result<Entry> exact = findWord(key);
    if (exact.ok() || exact.code != ErrorCode::WordNotFound) return exact;
    return findForm(key);
}

Result<std::vector<SuggestItem>> Dict::suggest(const std::string& prefix) {
    std::vector<SuggestItem> items;
    std::string key = normalizeWord(prefix);
    if (key.empty()) return Result<std::vector<SuggestItem>>::fail(ErrorCode::BadRequest);
    if (!ready_) return Result<std::vector<SuggestItem>>::fail(ErrorCode::DictNotReady);

    Statement query(db_,
        "SELECT id, word FROM dictionary_entries "
        "WHERE word LIKE ? ESCAPE '\\' COLLATE NOCASE "
        "ORDER BY word COLLATE NOCASE, id LIMIT ?");
    if (!query) return Result<std::vector<SuggestItem>>::fail(ErrorCode::DbError);
    std::string pattern = escapeLike(key) + "%";
    if (!bindText(query.get(), 1, pattern) || sqlite3_bind_int(query.get(), 2, kSuggestMax) != SQLITE_OK) {
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
    return findId(id);
}

Result<ImportReport> Dict::importFile(const std::string& path) {
    if (path.empty()) return Result<ImportReport>::fail(ErrorCode::BadRequest);
    if (!db_) return Result<ImportReport>::fail(ErrorCode::DbError);

    DatabaseHandle source;
    int openRc = sqlite3_open_v2(path.c_str(), &source.db, SQLITE_OPEN_READONLY, nullptr);
    if (openRc != SQLITE_OK || !source.db || !hasEcdictSchema(source.db)) {
        return Result<ImportReport>::fail(ErrorCode::BadRequest);
    }

    if (!execute(db_, "BEGIN IMMEDIATE")) return Result<ImportReport>::fail(ErrorCode::DbError);

    ImportReport report;
    Statement sourceEntries(source.db,
        "SELECT word, COALESCE(phonetic, ''), COALESCE(translation, ''), "
        "COALESCE(definition, '') FROM stardict ORDER BY id");
    Statement insertEntry(db_,
        "INSERT INTO dictionary_entries(word, phonetic, translation, definition) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(word) DO NOTHING");
    if (!sourceEntries || !insertEntry) {
        execute(db_, "ROLLBACK");
        return Result<ImportReport>::fail(ErrorCode::DbError);
    }

    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(sourceEntries.get())) == SQLITE_ROW) {
        std::string word = normalizeWord(columnText(sourceEntries.get(), 0));
        if (word.empty()) {
            ++report.failed;
            continue;
        }

        sqlite3_reset(insertEntry.get());
        sqlite3_clear_bindings(insertEntry.get());
        if (!bindText(insertEntry.get(), 1, word)
            || !bindText(insertEntry.get(), 2, columnText(sourceEntries.get(), 1))
            || !bindText(insertEntry.get(), 3, columnText(sourceEntries.get(), 2))
            || !bindText(insertEntry.get(), 4, columnText(sourceEntries.get(), 3))
            || sqlite3_step(insertEntry.get()) != SQLITE_DONE) {
            execute(db_, "ROLLBACK");
            return Result<ImportReport>::fail(ErrorCode::DbError);
        }
        if (sqlite3_changes(db_) == 1) ++report.added;
        else ++report.skipped;
    }
    if (rc != SQLITE_DONE) {
        execute(db_, "ROLLBACK");
        return Result<ImportReport>::fail(ErrorCode::DbError);
    }

    Statement sourceForms(source.db,
        "SELECT word, COALESCE(exchange, '') FROM stardict ORDER BY id");
    Statement findEntry(db_,
        "SELECT id FROM dictionary_entries WHERE word = ? COLLATE NOCASE LIMIT 1");
    Statement insertForm(db_,
        "INSERT INTO dictionary_forms(form, entry_id) VALUES(?, ?) "
        "ON CONFLICT(form) DO UPDATE SET entry_id = excluded.entry_id");
    if (!sourceForms || !findEntry || !insertForm) {
        execute(db_, "ROLLBACK");
        return Result<ImportReport>::fail(ErrorCode::DbError);
    }

    while ((rc = sqlite3_step(sourceForms.get())) == SQLITE_ROW) {
        std::string word = normalizeWord(columnText(sourceForms.get(), 0));
        std::vector<std::pair<std::string, std::string>> exchange =
            parseExchange(columnText(sourceForms.get(), 1));
        for (const auto& value : exchange) {
            if (value.first == "0") {
                if (!addForm(findEntry.get(), insertForm.get(), word, value.second)) {
                    execute(db_, "ROLLBACK");
                    return Result<ImportReport>::fail(ErrorCode::DbError);
                }
            } else if (value.first == "p" || value.first == "d" || value.first == "i"
                       || value.first == "3" || value.first == "s"
                       || value.first == "r" || value.first == "t") {
                for (const std::string& form : splitForms(value.second)) {
                    if (!addForm(findEntry.get(), insertForm.get(), form, word)) {
                        execute(db_, "ROLLBACK");
                        return Result<ImportReport>::fail(ErrorCode::DbError);
                    }
                }
            }
        }
    }
    if (rc != SQLITE_DONE || !execute(db_, "COMMIT")) {
        execute(db_, "ROLLBACK");
        return Result<ImportReport>::fail(ErrorCode::DbError);
    }

    if (!refreshReady()) {
        return Result<ImportReport>::fail(ErrorCode::DbError);
    }
    return Result<ImportReport>::success(report);
}
