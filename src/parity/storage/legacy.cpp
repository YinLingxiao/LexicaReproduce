#include "parity/storage/legacy.h"
#include <vector>
namespace lexica::parity {
namespace {
struct Source {
  sqlite3 *db = nullptr;
  explicit Source(const std::filesystem::path &path) {
    if (sqlite3_open_v2(path.u8string().c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK ||
        !db) {
      const std::string message = db ? sqlite3_errmsg(db) : "Cannot open source";
      sqlite3_close(db);
      throw Error("DB_ERROR", message);
    }
  }
  ~Source() { sqlite3_close(db); }
  Source(const Source &) = delete;
  Source &operator=(const Source &) = delete;
};
bool tableExists(sqlite3 *db, const char *name) {
  Statement query(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
  query.bind(1, name);
  return query.step();
}
std::int64_t countOf(sqlite3 *db, const char *table) {
  if (!tableExists(db, table))
    return -1;
  Statement query(db, std::string("SELECT COUNT(*) FROM ") + table);
  query.step();
  return query.integer(0);
}
std::int64_t userVersion(sqlite3 *db) {
  Statement query(db, "PRAGMA user_version");
  query.step();
  return query.integer(0);
}
std::string lowerAscii(std::string value) {
  for (char &character : value)
    if (character >= 'A' && character <= 'Z')
      character = static_cast<char>(character - 'A' + 'a');
  return value;
}
std::uintmax_t bytes(const std::filesystem::path &path) {
  std::error_code error;
  auto size = std::filesystem::file_size(path, error);
  if (error)
    throw Error("DB_ERROR", "Cannot stat the source database");
  return size;
}
} // namespace
Json inspectLegacy(const std::filesystem::path &source) {
  const auto before = bytes(source);
  Source input(source);
  const bool entries = tableExists(input.db, "dictionary_entries");
  const bool migrations = tableExists(input.db, "schema_migrations");
  const bool words = tableExists(input.db, "words");
  const auto version = userVersion(input.db);
  Json report{{"bytes", before}, {"user_version", version}};
  if (words && version > 0 && !entries) {
    report["source_kind"] = "upstream";
    report["source_modified"] = bytes(source) != before;
    return report;
  }
  if (!entries || !migrations)
    throw Error("SCHEMA_MISMATCH",
                "The file is not an old C++ Lexica database");
  report["source_kind"] = "legacy-cpp";
  report["dictionary_entries"] = countOf(input.db, "dictionary_entries");
  report["vocab_items"] = countOf(input.db, "vocab_item");
  report["review_states"] = countOf(input.db, "review_state");
  report["history_rows"] = countOf(input.db, "vocab_history");
  report["source_modified"] = bytes(source) != before;
  return report;
}
Json migrateLegacy(const std::filesystem::path &source, Database &target,
                   const Clock &clock) {
  auto report = inspectLegacy(source);
  if (report.value("source_kind", std::string{}) != "legacy-cpp")
    throw Error("SCHEMA_MISMATCH",
                "Refusing to convert an upstream Lexica database in place");
  const auto before = bytes(source);
  struct Row {
    std::string word;
    int favorite = 0;
    std::string note;
    int paused = 0;
  };
  std::vector<Row> rows;
  {
    Source input(source);
    if (!tableExists(input.db, "vocab_item")) {
      report["copied_notes"] = 0;
      report["copied_ignored"] = 0;
      report["skipped_missing_word"] = 0;
      report["skipped_existing"] = 0;
      report["unmapped"] = {
          {"review_state",
           "Legacy stage and due time are not Lexica memory events"},
          {"vocab_history",
           "Lookup and review logs are not converted into encounters"}};
      report["source_modified"] = bytes(source) != before;
      return report;
    }
    Statement query(input.db,
                    "SELECT word,favorite,note,paused FROM vocab_item");
    while (query.step())
      rows.push_back({query.text(0), static_cast<int>(query.integer(1)),
                      query.text(2), static_cast<int>(query.integer(3))});
  }
  int notes = 0, ignored = 0, missing = 0, existing = 0;
  const auto updated = formatUtc(clock.nowMillis());
  target.withConnection([&](sqlite3 *db) {
    Transaction transaction(db);
    for (const auto &row : rows) {
      Statement found(db, "SELECT id FROM words WHERE word=?");
      found.bind(1, lowerAscii(row.word));
      if (!found.step()) {
        ++missing;
        continue;
      }
      const auto id = found.integer(0);
      if (row.favorite || !row.note.empty()) {
        Statement insert(db, "INSERT INTO "
                             "word_notes(word_id,bookmarked,note,updated_at) "
                             "SELECT ?,?,?,? WHERE NOT EXISTS (SELECT 1 FROM "
                             "word_notes WHERE word_id=?)");
        insert.bind(1, id)
            .bind(2, static_cast<std::int64_t>(row.favorite ? 1 : 0))
            .bind(3, row.note)
            .bind(4, updated)
            .bind(5, id)
            .step();
        if (sqlite3_changes(db) == 1)
          ++notes;
        else
          ++existing;
      }
      if (row.paused) {
        Statement pause(db, "INSERT OR IGNORE INTO ignored_words(word_id) "
                            "VALUES(?)");
        pause.bind(1, id).step();
        if (sqlite3_changes(db) == 1)
          ++ignored;
      }
    }
    transaction.commit();
  });
  report["copied_notes"] = notes;
  report["copied_ignored"] = ignored;
  report["skipped_missing_word"] = missing;
  report["skipped_existing"] = existing;
  report["unmapped"] = {
      {"review_state",
       "Legacy stage and due time are not Lexica memory events"},
      {"vocab_history",
       "Lookup and review logs are not converted into encounters"}};
  report["source_modified"] = bytes(source) != before;
  return report;
}
} // namespace lexica::parity
