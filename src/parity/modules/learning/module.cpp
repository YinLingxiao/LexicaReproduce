#include "parity/modules/modules.h"
namespace lexica::parity {
void registerLearning(Router &router) {
  // Read-only Phase 0 query. C adds encounter creation + memory projection in
  // one transaction here. Do not call the legacy fixed-interval Review class.
  router.bind(
      "learning", "lookup_word",
      [](const Json &a, Context &c) -> Json {
        const auto args = a.get<LookupWordArgs>();
        return c.db.withConnection([&](sqlite3 *db) -> Json {
          LookupWordResult result;
          if (auto entry = c.dictionary.find(db, args.word))
            result.outcome = LookupOutcomeHit{*entry};
          else
            result.outcome =
                LookupOutcomeMiss{c.dictionary.suggest(db, args.word, 8)};
          return result;
        });
      },
      "read_only_seed");
}
} // namespace lexica::parity
