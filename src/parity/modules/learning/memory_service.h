#pragma once
#include "parity/modules/learning/memory_store.h"
namespace lexica::parity::learning {
struct DueWord {
  std::int64_t word_id = 0;
  bool high_priority = false;
  std::optional<UtcMillis> next_review_at;
  double retrievability = 0;
};
std::int64_t openVisit(sqlite3 *db, std::int64_t wordId, UtcMillis now);
void setComprehension(sqlite3 *db, std::int64_t encounterId, ComprehensionLevel level,
                      UtcMillis now);
std::optional<Projection> refreshWord(sqlite3 *db, std::int64_t wordId, UtcMillis now);
std::int64_t rebuildProjection(sqlite3 *db, UtcMillis now);
std::optional<MemoryState> loadState(sqlite3 *db, std::int64_t wordId);
std::vector<DueWord> dueWords(sqlite3 *db, UtcMillis now);
} // namespace lexica::parity::learning
