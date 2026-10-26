#include "parity/api/router.h"
#include "parity/modules/dictionary/api.h"
#include <fstream>
#include <memory>
namespace lexica::parity {
namespace {
class DictionaryEngine final : public DictionaryPort {
public:
  std::optional<WordEntry> byId(sqlite3 *db, std::int64_t id) override {
    return dictionaryById(db, id);
  }
  std::optional<WordEntry> find(sqlite3 *db, const std::string &input) override {
    return dictionaryFind(db, input);
  }
  std::vector<Suggestion> suggest(sqlite3 *db, const std::string &input, int limit) override {
    return dictionarySuggest(db, input, limit);
  }
};
std::int64_t ecdictCount(sqlite3 *db) {
  Statement query(db, "SELECT COUNT(*) FROM words WHERE source='ecdict'");
  query.step();
  return query.integer(0);
}
void writeReadyMarker(const std::filesystem::path &database) {
  const auto dir = database.parent_path();
  std::ofstream output(dir / "ecdict.ok", std::ios::binary);
  output << "1";
}
void emitSetup(Context &context, SetupPhase phase, const std::string &message) {
  if (!context.emit)
    return;
  SetupProgress event;
  event.phase = phase;
  event.message = message;
  context.emit("dictionary-setup", event);
}
} // namespace
std::unique_ptr<DictionaryPort> makeDictionary() {
  return std::make_unique<DictionaryEngine>();
}
void registerDictionary(Router &router) {
  router.bind("dictionary", "app_info", [](const Json &, Context &context) -> Json {
    return context.db.withConnection([&](sqlite3 *db) -> Json {
      Statement version(db, "PRAGMA user_version");
      version.step();
      Statement count(db, "SELECT COUNT(*) FROM words");
      count.step();
      bool fts = false;
      try {
        Statement probe(db, "SELECT rowid FROM words_fts LIMIT 1");
        probe.step();
        fts = true;
      } catch (const Error &) {
        fts = false;
      }
      return AppInfo{version.integer(0), count.integer(0), fts,
                     ecdictCount(db) >= kDictionaryReadyMinEntries};
    });
  });
  router.bind("dictionary", "search_suggest", [](const Json &args, Context &context) -> Json {
    const auto input = args.get<SearchSuggestArgs>();
    return context.db.withConnection([&](sqlite3 *db) -> Json {
      return context.dictionary.suggest(db, input.query,
                                        static_cast<int>(input.limit.value_or(8)));
    });
  });
  router.bind("dictionary", "import_ecdict", [](const Json &args, Context &context) -> Json {
    const auto input = args.get<ImportEcdictArgs>();
    const auto now = formatUtc(context.clock.nowMillis());
    auto database = openConfiguredDatabase(context.db.path());
    backfillSeedMetadata(database.get(), now);
    const auto imported = importEcdictDatabase(
        database.get(), std::filesystem::u8path(input.sourcePath), now);
    if (ecdictCount(database.get()) >= kDictionaryReadyMinEntries)
      writeReadyMarker(context.db.path());
    return Json(imported);
  });
  router.bind("dictionary", "ensure_dictionary", [](const Json &, Context &context) -> Json {
    try {
      return ensureDictionary(context);
    } catch (const Error &error) {
      emitSetup(context, SetupPhase::Error, error.what());
      throw;
    } catch (const std::exception &error) {
      emitSetup(context, SetupPhase::Error, error.what());
      throw Error("INTERNAL_ERROR", error.what());
    }
  });
}
} // namespace lexica::parity
