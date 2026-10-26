#include "parity/storage/upstream.h"
#include "parity/storage/database.h"
namespace lexica::parity {
namespace {
std::string uriOf(const std::filesystem::path &file) {
  std::string path = file.u8string();
  for (char &character : path)
    if (character == '\\')
      character = '/';
  return "file:///" + path + "?mode=ro&immutable=1";
}
} // namespace
Json probeUpstreamDatabase(const std::filesystem::path &file) {
  std::error_code error;
  const auto before = std::filesystem::file_size(file, error);
  if (error)
    throw Error("DB_ERROR", "Cannot stat the upstream database");
  const auto beforeTime = std::filesystem::last_write_time(file, error);
  sqlite3 *db = nullptr;
  auto code = sqlite3_open_v2(uriOf(file).c_str(), &db,
                              SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
  if (code != SQLITE_OK || !db) {
    const std::string message = db ? sqlite3_errmsg(db) : "Cannot open upstream database";
    sqlite3_close(db);
    throw Error("DB_ERROR", message);
  }
  Json report{{"bytes", before}, {"opened_readonly", true}};
  try {
    Statement version(db, "PRAGMA user_version");
    version.step();
    report["user_version"] = version.integer(0);
    const char *required[] = {
        "words",           "senses",           "examples",
        "collocations",    "word_forms",       "words_fts",
        "encounters",      "memory_states",    "ignored_words",
        "reviews",         "review_attempts",  "word_notes",
        "app_settings",    "ai_example_cache", "word_enrichments",
        "custom_wordbook_words"};
    Json missing = Json::array();
    for (const char *name : required) {
      Statement query(db, "SELECT 1 FROM sqlite_master WHERE name=?");
      query.bind(1, name);
      if (!query.step())
        missing.push_back(name);
    }
    report["missing"] = missing;
    report["tables_ok"] = missing.empty();
    Statement sample(db, "SELECT word FROM words WHERE word=? LIMIT 1");
    sample.bind(1, "meticulous");
    report["sample_word"] = sample.step() ? Json(sample.text(0)) : Json(nullptr);
  } catch (...) {
    sqlite3_close(db);
    throw;
  }
  sqlite3_close(db);
  const auto after = std::filesystem::file_size(file, error);
  const auto afterTime = std::filesystem::last_write_time(file, error);
  report["bytes_after"] = after;
  report["mutated"] = after != before || afterTime != beforeTime;
  return report;
}
} // namespace lexica::parity
