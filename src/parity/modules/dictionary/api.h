#pragma once
#include "parity/contracts/ports.h"
#include "parity/modules/dictionary/normalize.h"
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>
namespace lexica::parity {
inline constexpr std::int64_t kDictionaryReadyMinEntries = 1000000;
inline constexpr std::size_t kEcdictImportBatch = 20000;
inline const char *mapFormKind(const std::string &raw) {
  const auto kind = trimCopy(raw);
  if (kind == "s" || kind == "plural" || kind == "Plural")
    return "Plural";
  if (kind == "p" || kind == "past" || kind == "Past")
    return "Past";
  if (kind == "d" || kind == "past_participle" || kind == "Past participle")
    return "Past participle";
  if (kind == "i" || kind == "ing" || kind == "present_participle" ||
      kind == "Present participle")
    return "Present participle";
  if (kind == "3" || kind == "third_person" || kind == "3rd_person" ||
      kind == "3rd person singular")
    return "3rd person singular";
  if (kind == "r" || kind == "comparative" || kind == "Comparative")
    return "Comparative";
  if (kind == "t" || kind == "superlative" || kind == "Superlative")
    return "Superlative";
  if (kind == "noun" || kind == "Noun")
    return "Noun";
  if (kind == "verb" || kind == "Verb")
    return "Verb";
  if (kind == "adverb" || kind == "Adverb")
    return "Adverb";
  if (kind == "adjective" || kind == "Adjective")
    return "Adjective";
  return nullptr;
}
struct SeedRecord {
  std::string word;
  std::optional<std::string> phonetic;
  std::optional<std::int64_t> frequency;
  std::string synonyms = "[]";
  std::string antonyms = "[]";
  std::string wordFamily = "[]";
  std::string tags;
  std::optional<std::int64_t> collins;
  bool oxford = false;
  std::optional<std::string> mnemonic;
  std::vector<RootPart> roots;
  std::vector<std::pair<std::string, std::string>> forms;
};
const SeedRecord *findSeedRecord(const std::string &normalizedWord);
struct OwnedConnection {
  sqlite3 *db = nullptr;
  OwnedConnection() = default;
  explicit OwnedConnection(sqlite3 *database) : db(database) {}
  ~OwnedConnection() {
    if (db)
      sqlite3_close(db);
  }
  OwnedConnection(const OwnedConnection &) = delete;
  OwnedConnection &operator=(const OwnedConnection &) = delete;
  OwnedConnection(OwnedConnection &&other) noexcept : db(other.db) { other.db = nullptr; }
  OwnedConnection &operator=(OwnedConnection &&other) noexcept {
    if (this != &other) {
      if (db)
        sqlite3_close(db);
      db = other.db;
      other.db = nullptr;
    }
    return *this;
  }
  sqlite3 *get() const { return db; }
};
OwnedConnection openConfiguredDatabase(const std::filesystem::path &path);
struct EcdictImportHooks {
  std::size_t batchSize = kEcdictImportBatch;
  std::function<void()> beforeCommit;
  std::function<void()> betweenBatches;
};
struct EnsureOptions {
  std::vector<std::string> urls;
};
std::int64_t importEcdictDatabase(sqlite3 *dest,
                                  const std::filesystem::path &source,
                                  const std::string &createdAt,
                                  const EcdictImportHooks &hooks = {});
EnsureDictionaryResult ensureDictionary(Context &context,
                                        const EnsureOptions &options = {});
void backfillSeedMetadata(sqlite3 *db, const std::string &now);
std::optional<WordEntry> dictionaryFind(sqlite3 *db, const std::string &input);
std::optional<WordEntry> dictionaryById(sqlite3 *db, std::int64_t id);
std::vector<Suggestion> dictionarySuggest(sqlite3 *db, const std::string &query,
                                          int limit);
}
