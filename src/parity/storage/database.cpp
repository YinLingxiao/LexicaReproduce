#include "parity/storage/database.h"
#include "parity/contracts/clock.h"
#include "parity/contracts/dto.h"
#include "parity_resources.h"
#include <iterator>
namespace lexica::parity {
namespace {
void check(sqlite3 *db, int code) {
  if (code != SQLITE_OK)
    throw Error("DB_ERROR", sqlite3_errmsg(db));
}
void migrate(sqlite3 *db) {
  std::int64_t current = 0;
  {
    Statement version(db, "PRAGMA user_version");
    version.step();
    current = version.integer(0);
  }
  if (current > static_cast<std::int64_t>(std::size(kMigrations)))
    throw Error("VERSION_TOO_NEW",
                "Database schema is newer than this application");
  if (current == 0) {
    Statement tables(db, "SELECT COUNT(*) FROM sqlite_master WHERE "
                         "type='table' AND name NOT LIKE 'sqlite_%'");
    tables.step();
    if (tables.integer(0))
      throw Error(
          "SCHEMA_MISMATCH",
          "Non-empty unversioned database; use a separate parity database");
  }
  for (auto i = current; i < static_cast<std::int64_t>(std::size(kMigrations));
       ++i) {
    Transaction tx(db);
    execute(db, kMigrations[i]);
    execute(db, "PRAGMA user_version=" + std::to_string(i + 1));
    tx.commit();
  }
}
} // namespace
void execute(sqlite3 *db, const std::string &sql) {
  check(db, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr));
}
Statement::Statement(sqlite3 *db, const std::string &sql) {
  check(db, sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr));
}
Statement::~Statement() { sqlite3_finalize(stmt_); }
Statement &Statement::bind(int i, const std::string &v) {
  check(sqlite3_db_handle(stmt_),
        sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()),
                          SQLITE_TRANSIENT));
  return *this;
}
Statement &Statement::bind(int i, std::int64_t v) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_int64(stmt_, i, v));
  return *this;
}
Statement &Statement::null(int i) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_null(stmt_, i));
  return *this;
}
bool Statement::step() {
  auto code = sqlite3_step(stmt_);
  if (code == SQLITE_ROW)
    return true;
  if (code == SQLITE_DONE)
    return false;
  throw Error("DB_ERROR", sqlite3_errmsg(sqlite3_db_handle(stmt_)));
}
std::int64_t Statement::integer(int col) const {
  return sqlite3_column_int64(stmt_, col);
}
std::string Statement::text(int col) const {
  auto t = sqlite3_column_text(stmt_, col);
  return t ? std::string(reinterpret_cast<const char *>(t),
                         sqlite3_column_bytes(stmt_, col))
           : std::string{};
}
bool Statement::isNull(int col) const {
  return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}
Transaction::Transaction(sqlite3 *db) : db_(db) {
  execute(db_, "BEGIN IMMEDIATE");
}
Transaction::~Transaction() {
  if (!committed_)
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}
void Transaction::commit() {
  execute(db_, "COMMIT");
  committed_ = true;
}
Database::Database(const std::filesystem::path &path) : path_(path) {
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  const auto code = sqlite3_open_v2(path.u8string().c_str(), &db_,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                        SQLITE_OPEN_FULLMUTEX,
                                    nullptr);
  if (code != SQLITE_OK) {
    const std::string message =
        db_ ? sqlite3_errmsg(db_) : "Cannot open database";
    sqlite3_close(db_);
    db_ = nullptr;
    throw Error("DB_ERROR", message);
  }
  try {
    sqlite3_busy_timeout(db_, 2000);
    execute(db_, "PRAGMA foreign_keys=ON");
    migrate(db_);
    execute(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  } catch (...) {
    sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}
Database::~Database() {
  if (db_)
    sqlite3_close(db_);
}
void Database::seed() {
  withConnection([](sqlite3 *db) {
    const auto seed = Json::parse(kSeedJson);
    Transaction tx(db);
    for (const auto &word : seed.at("words")) {
      Statement exists(db, "SELECT id FROM words WHERE word=?");
      exists.bind(1, word.at("word").get<std::string>());
      if (exists.step())
        continue; // Idempotent; never overwrite user/imported content.
      Statement insert(
          db, "INSERT INTO "
              "words(word,display,phonetic,frequency_rank,synonyms,antonyms,"
              "word_family,source,created_at) VALUES(?,?,?,?,?,?,?,'seed',?)");
      const auto name = word.at("word").get<std::string>();
      insert.bind(1, name).bind(2, name).bind(
          3, word.value("phonetic", std::string{}));
      if (word.contains("frequency_rank") && !word["frequency_rank"].is_null())
        insert.bind(4, word["frequency_rank"].get<std::int64_t>());
      else
        insert.null(4);
      insert.bind(5, word.value("synonyms", Json::array()).dump())
          .bind(6, word.value("antonyms", Json::array()).dump())
          .bind(7, word.value("word_family", Json::array()).dump())
          .bind(8, "2026-10-21T00:00:00.000Z")
          .step();
      const auto id = sqlite3_last_insert_rowid(db);
      int order = 0;
      for (const auto &sense : word.at("senses")) {
        Statement s(db, "INSERT INTO "
                        "senses(word_id,pos,english_definition,chinese_"
                        "definition,level,order_idx) VALUES(?,?,?,?,?,?)");
        s.bind(1, id);
        for (const auto &field :
             std::initializer_list<std::pair<int, const char *>>{
                 {2, "pos"}, {3, "english"}, {4, "chinese"}, {5, "level"}}) {
          if (sense.contains(field.second) && !sense[field.second].is_null())
            s.bind(field.first, sense[field.second].get<std::string>());
          else if (field.first == 3)
            s.bind(3, "");
          else
            s.null(field.first);
        }
        s.bind(6, order++).step();
        const auto sid = sqlite3_last_insert_rowid(db);
        int eo = 0;
        for (const auto &ex : sense.value("examples", Json::array())) {
          Statement e(
              db, "INSERT INTO examples(sense_id,text,translation,order_idx) "
                  "VALUES(?,?,?,?)");
          e.bind(1, sid).bind(2, ex.at("text").get<std::string>());
          if (ex.contains("translation") && !ex["translation"].is_null())
            e.bind(3, ex["translation"].get<std::string>());
          else
            e.null(3);
          e.bind(4, eo++).step();
        }
      }
      int co = 0;
      for (const auto &c : word.value("collocations", Json::array())) {
        Statement q(db,
                    "INSERT INTO collocations(word_id,text,gloss,order_idx) "
                    "VALUES(?,?,?,?)");
        q.bind(1, id).bind(2, c.at("text").get<std::string>());
        if (c.contains("gloss") && !c["gloss"].is_null())
          q.bind(3, c["gloss"].get<std::string>());
        else
          q.null(3);
        q.bind(4, co++).step();
      }
      const auto forms = word.value("forms", Json::object());
      for (const auto &item : forms.items()) {
        Statement f(db, "INSERT OR IGNORE INTO word_forms(form,word_id,kind) "
                        "VALUES(?,?,?)");
        f.bind(1, item.value().get<std::string>())
            .bind(2, id)
            .bind(3, item.key())
            .step();
      }
    }
    tx.commit();
  });
}
} // namespace lexica::parity
