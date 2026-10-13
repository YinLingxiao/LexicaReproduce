// 词库模块：管理共享数据库的建表升级、词库导入、精确查词和词形回退。
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
constexpr int kVocabSchemaVersion = 2;
constexpr int kBatchRows = 5000;

// SQL 语句资源包装：构造时准备语句，离开作用域时自动释放。
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
    // 返回已准备好的 SQL 语句句柄，供绑定参数和逐行执行使用。
    sqlite3_stmt* get() const { return stmt_; }
    explicit operator bool() const { return stmt_ != nullptr; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// 执行 SQL；事务、建表等操作通过它检查执行是否成功。
bool execute(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
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

// 把数据库查询的一行转换成程序使用的 Entry 词条。
Entry readEntry(sqlite3_stmt* stmt) {
    Entry entry;
    entry.id = sqlite3_column_int64(stmt, 0);
    entry.word = columnText(stmt, 1);
    entry.phonetic = columnText(stmt, 2);
    entry.translation = columnText(stmt, 3);
    entry.definition = columnText(stmt, 4);
    return entry;
}

// 删除字符串开头和结尾的空白，保留中间内容。
std::string trim(const std::string& in) {
    std::size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) ++begin;
    std::size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) --end;
    return in.substr(begin, end - begin);
}

// 读取当前毫秒时间，用作任务或题目编号的起点。
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// 执行 SQLite 的快速完整性检查，确认数据库没有明显损坏。
bool integrityOk(sqlite3* db) {
    Statement check(db, "PRAGMA quick_check(1)");
    if (!check || sqlite3_step(check.get()) != SQLITE_ROW) return false;
    return columnText(check.get(), 0) == "ok";
}

// 读取某个模块已登记的最高数据库版本；ok 表示查询是否成功。
int moduleVersion(sqlite3* db, const char* module, bool& ok) {
    Statement query(db, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations WHERE module = ?");
    if (!query || !bindText(query.get(), 1, module) || sqlite3_step(query.get()) != SQLITE_ROW) {
        ok = false;
        return 0;
    }
    ok = true;
    return sqlite3_column_int(query.get(), 0);
}

// 在事务中检查数据库版本，并按版本顺序补齐词库、复习、个人词汇表。
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
    bool vocabOk = false;
    int vocabVersion = reviewOk ? moduleVersion(db, "vocab", vocabOk) : 0;
    Statement modules(db, "SELECT DISTINCT module FROM schema_migrations");
    bool foreign = false;
    while (modules && sqlite3_step(modules.get()) == SQLITE_ROW) {
        std::string name = columnText(modules.get(), 0);
        if (name != "dict" && name != "review" && name != "vocab") foreign = true;
    }
    if (!ok || !reviewOk || !vocabOk || !modules) {
        execute(db, "ROLLBACK");
        return StartupStatus::DbCorrupt;
    }
    // 发现本程序不认识的模块或更高版本时拒绝升级，避免误改新版本数据。
    if (foreign || dictVersion > kDictSchemaVersion || reviewVersion > kReviewSchemaVersion || vocabVersion > kVocabSchemaVersion) {
        execute(db, "ROLLBACK");
        return StartupStatus::VersionTooNew;
    }
    // 迁移 1：创建词条表，以及“变形词→原型词”的关系表。
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
    // 迁移 2：保留来源中的原始拼写，供大小写冲突时决定采用哪条记录。
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
    // 迁移 3：保存每个词的学习阶段与下次到期时间。
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
    // 迁移 4：创建个人词汇、活动历史和查词请求去重表。
    if (vocabVersion < 1) {
        const char* sql =
            "CREATE TABLE vocab_item ("
            "entry_id INTEGER PRIMARY KEY REFERENCES dictionary_entries(id) ON DELETE CASCADE,"
            "word TEXT NOT NULL DEFAULT '',"
            "favorite INTEGER NOT NULL DEFAULT 0 CHECK(favorite IN(0,1)),"
            "note TEXT NOT NULL DEFAULT '',"
            "paused INTEGER NOT NULL DEFAULT 0 CHECK(paused IN(0,1)));"
            "CREATE TABLE vocab_history ("
            "seq INTEGER PRIMARY KEY AUTOINCREMENT,"
            "entry_id INTEGER NOT NULL REFERENCES dictionary_entries(id) ON DELETE CASCADE,"
            "utc INTEGER NOT NULL,"
            "kind TEXT NOT NULL CHECK(kind IN('lookup','review')),"
            "qid INTEGER);"
            "CREATE INDEX vocab_history_kind_seq ON vocab_history(kind, seq DESC);"
            "CREATE INDEX vocab_history_utc ON vocab_history(utc);"
            "CREATE UNIQUE INDEX vocab_history_qid ON vocab_history(qid) WHERE qid IS NOT NULL;"
            "CREATE TABLE vocab_lookup_op ("
            "op_id TEXT PRIMARY KEY,"
            "entry_id INTEGER NOT NULL REFERENCES dictionary_entries(id) ON DELETE CASCADE,"
            "utc INTEGER NOT NULL);"
            "INSERT INTO schema_migrations(id, module, version, name) "
            "VALUES(4, 'vocab', 1, 'create personal vocabulary tables')";
        if (!execute(db, sql)) {
            execute(db, "ROLLBACK");
            return StartupStatus::DbCorrupt;
        }
    }
    // 迁移 5：正式复习事件按题目编号去重，失败后可以补记且不会重复计数。
    if (vocabVersion < 2) {
        if (vocabVersion >= 1) {
            const char* sql =
                "ALTER TABLE vocab_history ADD COLUMN qid INTEGER;"
                "CREATE UNIQUE INDEX vocab_history_qid ON vocab_history(qid) WHERE qid IS NOT NULL;";
            if (!execute(db, sql)) {
                execute(db, "ROLLBACK");
                return StartupStatus::DbCorrupt;
            }
        }
        const char* mark =
            "INSERT INTO schema_migrations(id, module, version, name) "
            "VALUES(5, 'vocab', 2, 'dedupe review events by question')";
        if (!execute(db, mark)) {
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

// 从外部词库读到的一条原始记录，尚未写入本地词库。
struct SourceRow {
    std::string word;
    std::string phonetic;
    std::string definition;
    std::string translation;
    std::string exchange;
};

// 统一源文件读取方式，使导入流程不用关心源文件格式。
class Source {
public:
    virtual ~Source() = default;
    // 读取源文件下一条词条；1=有记录，0=结束，-1=错误。
    virtual int next(SourceRow& row) = 0;
};

// 读取 ECDICT SQLite 文件的 stardict 表。
class SqliteSource : public Source {
public:
    ~SqliteSource() override {
        if (st_) sqlite3_finalize(st_);
        if (db_) sqlite3_close(db_);
    }
    // 打开源词库并检查必要的表或字段，成功后准备逐条读取。
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
    // 读取源文件下一条词条；1=有记录，0=结束，-1=错误。
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

// 按表头定位字段，读取 ECDICT CSV 文件。
class CsvSource : public Source {
public:
    // 打开源词库并检查必要的表或字段，成功后准备逐条读取。
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
    // 读取源文件下一条词条；1=有记录，0=结束，-1=错误。
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

    // 按列号取 CSV 字段；这一行缺少该列时返回空串。
    static std::string field(const std::vector<std::string>& fields, int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= fields.size()) return "";
        return fields[static_cast<std::size_t>(index)];
    }
    // 把 CSV 字段里的字面量反斜杠转义还原为换行或反斜杠。
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
    // 读取一整条 CSV 记录，处理逗号、引号和引号内的换行；1=有记录，0=结束，-1=错误。
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

// 检查文件头，自动选择 SQLite 或 CSV 读取器，并校验必需字段。
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

// 拆开 ECDICT 的词形字段，例如 i:running/p:ran，得到类型和值。
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

// 拆开逗号分隔的多个词形，并统一大小写与两端空白。
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

// 查询临时表或正式表里有没有这个词；1=有，0=没有，-1=查询失败。
int wordExists(sqlite3_stmt* stmt, const std::string& word) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (!bindText(stmt, 1, word)) return -1;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

// 把一条词写入临时表，沿用正式导入的大小写和更新规则，不改正式词库。
bool stageWord(sqlite3_stmt* findStage, sqlite3_stmt* findLive, sqlite3_stmt* insertStage, sqlite3_stmt* updateStage, const SourceRow& row,
               ImportReport& report) {
    const std::string raw = trim(row.word);
    const std::string word = normalizeWord(raw);
    if (word.empty()) {
        ++report.failed;
        return true;
    }
    const int staged = wordExists(findStage, word);
    if (staged < 0) return false;
    std::string oldRaw;
    std::string oldPhonetic;
    std::string oldTranslation;
    std::string oldDefinition;
    bool inLive = false;
    if (staged == 1) {
        oldRaw = columnText(findStage, 0);
        oldPhonetic = columnText(findStage, 1);
        oldTranslation = columnText(findStage, 2);
        oldDefinition = columnText(findStage, 3);
    } else {
        const int live = wordExists(findLive, word);
        if (live < 0) return false;
        if (live == 1) {
            inLive = true;
            oldRaw = columnText(findLive, 0);
            oldPhonetic = columnText(findLive, 1);
            oldTranslation = columnText(findLive, 2);
            oldDefinition = columnText(findLive, 3);
        }
    }
    if (staged == 1 || inLive) {
        // 遇到只有大小写不同的词，已有的小写原拼写优先于大写记录。
        const bool oldLower = oldRaw.empty() || oldRaw == word;
        if (oldLower && raw != word) {
            ++report.skipped;
            return true;
        }
        if (oldRaw == raw && oldPhonetic == row.phonetic && oldTranslation == row.translation && oldDefinition == row.definition) {
            ++report.skipped;
            return true;
        }
        ++report.updated;
        sqlite3_stmt* dest = staged == 1 ? updateStage : insertStage;
        sqlite3_reset(dest);
        sqlite3_clear_bindings(dest);
        if (!bindText(dest, 1, raw) || !bindText(dest, 2, row.phonetic) || !bindText(dest, 3, row.translation) || !bindText(dest, 4, row.definition)) {
            return false;
        }
        if (staged == 1) {
            if (!bindText(dest, 5, word)) return false;
        } else {
            if (!bindText(dest, 5, word) || sqlite3_bind_int(dest, 6, 0) != SQLITE_OK) return false;
        }
        return sqlite3_step(dest) == SQLITE_DONE;
    }
    sqlite3_reset(insertStage);
    sqlite3_clear_bindings(insertStage);
    if (!bindText(insertStage, 1, raw) || !bindText(insertStage, 2, row.phonetic) || !bindText(insertStage, 3, row.translation) ||
        !bindText(insertStage, 4, row.definition) || !bindText(insertStage, 5, word) || sqlite3_bind_int(insertStage, 6, 1) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_step(insertStage) != SQLITE_DONE) return false;
    ++report.added;
    return true;
}

// 把词形关系写入临时表；原型既不在临时表也不在正式表时跳过。
bool stageForm(sqlite3_stmt* findStage, sqlite3_stmt* findLive, sqlite3_stmt* insertForm, const std::string& rawForm, const std::string& rawLemma) {
    const std::string form = normalizeWord(rawForm);
    const std::string lemma = normalizeWord(rawLemma);
    if (form.empty() || lemma.empty() || form == lemma) return true;
    const int staged = wordExists(findStage, lemma);
    if (staged < 0) return false;
    if (staged == 0) {
        const int live = wordExists(findLive, lemma);
        if (live < 0) return false;
        if (live == 0) return true;
    }
    sqlite3_reset(insertForm);
    sqlite3_clear_bindings(insertForm);
    if (!bindText(insertForm, 1, form) || !bindText(insertForm, 2, lemma)) return false;
    return sqlite3_step(insertForm) == SQLITE_DONE;
}

// 临时表校验完成后，在一个事务里更新正式词条并保留已有编号。
bool applyStaged(sqlite3* db) {
    if (!execute(db, "BEGIN IMMEDIATE")) return false;
    const char* sql =
        "UPDATE dictionary_entries SET "
        "raw=(SELECT raw FROM stage_entry s WHERE s.word=dictionary_entries.word),"
        "phonetic=(SELECT phonetic FROM stage_entry s WHERE s.word=dictionary_entries.word),"
        "translation=(SELECT translation FROM stage_entry s WHERE s.word=dictionary_entries.word),"
        "definition=(SELECT definition FROM stage_entry s WHERE s.word=dictionary_entries.word) "
        "WHERE word IN (SELECT word FROM stage_entry WHERE is_new=0);"
        "INSERT INTO dictionary_entries(word, raw, phonetic, translation, definition) "
        "SELECT word, raw, phonetic, translation, definition FROM stage_entry WHERE is_new=1;"
        "INSERT INTO dictionary_forms(form, entry_id) "
        "SELECT f.form, e.id FROM stage_form f JOIN dictionary_entries e ON e.word=f.lemma "
        "ON CONFLICT(form) DO UPDATE SET entry_id=excluded.entry_id;";
    if (!execute(db, sql) || !execute(db, "COMMIT")) {
        execute(db, "ROLLBACK");
        return false;
    }
    return true;
}

// 提交当前导入批次；成功后把事务标记设为已关闭。
bool commitBatch(sqlite3* db, bool& open) {
    if (!open) return true;
    if (!execute(db, "COMMIT")) return false;
    open = false;
    return true;
}

// 用独立连接把数据先导入临时表；全部通过后才一次性写入正式词库。
ErrorCode importRows(const std::string& dbFile, const std::string& srcPath, const std::atomic<bool>& stop, ImportReport& report,
                      const std::function<void(const ImportReport&)>& progress) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbFile.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return ErrorCode::DbError;
    }
    sqlite3_busy_timeout(db, 5000);
    execute(db, "PRAGMA foreign_keys = ON");
    const char* stageSql =
        "CREATE TEMP TABLE stage_entry ("
        "word TEXT PRIMARY KEY,"
        "raw TEXT NOT NULL,"
        "phonetic TEXT NOT NULL,"
        "translation TEXT NOT NULL,"
        "definition TEXT NOT NULL,"
        "is_new INTEGER NOT NULL);"
        "CREATE TEMP TABLE stage_form ("
        "form TEXT PRIMARY KEY,"
        "lemma TEXT NOT NULL);";
    ErrorCode code = execute(db, stageSql) ? ErrorCode::Ok : ErrorCode::DbError;
    // 分两遍读取：先确保所有原型词存在，再建立词形映射，避免依赖源文件顺序。
    for (int pass = 0; pass < 2 && code == ErrorCode::Ok; ++pass) {
        ErrorCode srcCode = ErrorCode::Ok;
        std::unique_ptr<Source> src = openSource(srcPath, srcCode);
        if (!src) {
            code = srcCode == ErrorCode::BadRequest && pass == 1 ? ErrorCode::DbError : srcCode;
            break;
        }
        Statement findStage(db, pass == 0 ? "SELECT raw, phonetic, translation, definition FROM stage_entry WHERE word=?"
                                          : "SELECT 1 FROM stage_entry WHERE word=?");
        Statement findLive(db, "SELECT raw, phonetic, translation, definition FROM dictionary_entries WHERE word=?");
        Statement insertStage(db, "INSERT INTO stage_entry(raw, phonetic, translation, definition, word, is_new) VALUES(?,?,?,?,?,?)");
        Statement updateStage(db, "UPDATE stage_entry SET raw=?, phonetic=?, translation=?, definition=? WHERE word=?");
        Statement insertForm(db, "INSERT INTO stage_form(form, lemma) VALUES(?, ?) ON CONFLICT(form) DO UPDATE SET lemma=excluded.lemma");
        if (!findStage || !findLive || (pass == 0 && (!insertStage || !updateStage)) || (pass == 1 && !insertForm)) {
            code = ErrorCode::DbError;
            break;
        }
        bool tx = false;
        int rows = 0;
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
            // 第一遍只写临时词条：已有词保留编号，新词等整份文件通过后再分配编号。
            if (pass == 0) {
                if (!stageWord(findStage.get(), findLive.get(), insertStage.get(), updateStage.get(), row, report)) code = ErrorCode::DbError;
            } else {
                const std::string word = normalizeWord(row.word);
                if (!word.empty()) {
                    // 第二遍：0 表示原型，p/d/i/3/s/r/t 等表示过去式、分词或其他词形。
                    for (const auto& value : parseExchange(row.exchange)) {
                        if (value.first == "0") {
                            if (!stageForm(findStage.get(), findLive.get(), insertForm.get(), word, value.second)) code = ErrorCode::DbError;
                        } else if (value.first == "p" || value.first == "d" || value.first == "i" || value.first == "3" ||
                                   value.first == "s" || value.first == "r" || value.first == "t") {
                            for (const std::string& form : splitForms(value.second)) {
                                if (!stageForm(findStage.get(), findLive.get(), insertForm.get(), form, word)) code = ErrorCode::DbError;
                            }
                        }
                    }
                }
            }
            // 每 5000 行提交一次临时表，缩短写锁占用；正式词库要等整份文件通过后才改。
            if (code == ErrorCode::Ok && ++rows >= kBatchRows) {
                if (!commitBatch(db, tx)) code = ErrorCode::DbError;
                else if (progress) progress(report);
                rows = 0;
            }
        }
        if (code == ErrorCode::Ok && tx && !commitBatch(db, tx)) code = ErrorCode::DbError;
        if (tx) execute(db, "ROLLBACK");
    }
    if (code == ErrorCode::Ok && !applyStaged(db)) code = ErrorCode::DbError;
    if (code != ErrorCode::Ok) report = ImportReport{};
    sqlite3_close(db);
    if (progress) progress(report);
    return code;
}

}

// 把启动状态转成网页和控制台使用的固定名称。
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

// 关闭数据库连接，释放本模块持有的资源。
void Dict::close() {
    stop_ = true;
    // 先通知后台导入停止并等它结束，再关闭它可能还在使用的数据库。
    if (worker_.joinable()) worker_.join();
    stop_ = false;
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    ready_ = false;
}

// 返回词库是否有词条；后台导入也会更新这个状态。
bool Dict::ready() const { return ready_; }

// 从数据库检查是否至少有一个词条，并刷新 ready 标记。
bool Dict::refreshReady() {
    if (!db_) return false;
    Statement count(db_, "SELECT EXISTS(SELECT 1 FROM dictionary_entries LIMIT 1)");
    if (!count || sqlite3_step(count.get()) != SQLITE_ROW) return false;
    ready_ = sqlite3_column_int(count.get(), 0) != 0;
    return true;
}

// 打开共享数据库，检查完整性与版本，完成迁移后启用外键和 WAL 日志。
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

// 先按规范化词头精确查找；找不到时通过词形表回退到原型。
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
    // 精确查不到才尝试词形回退，例如 running 回到 run。
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

// 按输入前缀取最多 8 个候选词；这一步不算正式查词。
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

// 按词条编号读取完整内容。
Result<Entry> Dict::get(EntryId id) {
    if (!ready_) return Result<Entry>::fail(ErrorCode::DictNotReady);
    Statement query(db_, "SELECT id, word, phonetic, translation, definition FROM dictionary_entries WHERE id = ? LIMIT 1");
    if (!query || sqlite3_bind_int64(query.get(), 1, id) != SQLITE_OK) return Result<Entry>::fail(ErrorCode::DbError);
    int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return Result<Entry>::success(readEntry(query.get()));
    if (rc == SQLITE_DONE) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::fail(ErrorCode::DbError);
}

// 读取这个原型词对应的全部词形，供复习题遮住答案使用。
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

// 同步读取词库并写入数据库，完成后刷新词库是否可用的状态。
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

// 启动后台导入并立即返回任务；已有导入在运行时返回当前任务。
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
    // 后台线程负责读文件和写库；网页通过 importTask 轮询进度。
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

// 按任务编号查询导入状态和累计处理数量。
Result<ImportTask> Dict::importTask(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (id.empty() || id != task_.id) return Result<ImportTask>::fail(ErrorCode::BadRequest);
    return Result<ImportTask>::success(task_);
}
