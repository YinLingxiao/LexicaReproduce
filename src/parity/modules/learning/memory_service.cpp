#include "parity/modules/learning/memory_service.h"
#include "parity/storage/database.h"
#include <algorithm>
namespace lexica::parity::learning {
std::optional<Projection> refreshWord(sqlite3 *db, std::int64_t wordId, UtcMillis now) {
  auto state = fold(wordId, loadEvents(db, wordId));
  if (state)
    upsertState(db, *state, now);
  return state;
}
std::int64_t openVisit(sqlite3 *db, std::int64_t wordId, UtcMillis now) {
  std::int64_t id = 0;
  if (auto latest = latestVisit(db, wordId);
      latest && now >= latest->at && now - latest->at < kVisitMergeMs) {
    updateVisitTime(db, latest->id, now);
    id = latest->id;
  } else
    id = insertVisit(db, wordId, now);
  refreshWord(db, wordId, now);
  return id;
}
void setComprehension(sqlite3 *db, std::int64_t encounterId, ComprehensionLevel level,
                      UtcMillis now) {
  auto encounter = getEncounter(db, encounterId);
  if (!encounter)
    throw Error("memory",
                "encounter " + std::to_string(encounterId) + " not found");
  auto latest = latestVisit(db, encounter->word_id);
  if (!latest || latest->id != encounterId)
    throw Error("memory", "encounter " + std::to_string(encounterId) +
                               " is not the latest for its word");
  if (level != encounter->level) {
    const auto escalates = encounter->level == ComprehensionLevel::Unknown ||
                           helpRank(level) > helpRank(encounter->level);
    if (!escalates)
      throw Error("memory", std::string("comprehension cannot change from ") +
                                levelName(encounter->level) + " to " +
                                levelName(level));
    updateLevel(db, encounterId, level);
  }
  refreshWord(db, encounter->word_id, now);
}
std::int64_t rebuildProjection(sqlite3 *db, UtcMillis now) {
  Transaction tx(db);
  std::int64_t count = 0;
  for (auto wordId : wordsWithEvents(db)) {
    refreshWord(db, wordId, now);
    ++count;
  }
  tx.commit();
  return count;
}
std::optional<MemoryState> loadState(sqlite3 *db, std::int64_t wordId) {
  auto state = loadProjection(db, wordId);
  if (!state)
    return std::nullopt;
  return toDto(*state);
}
std::vector<DueWord> dueWords(sqlite3 *db, UtcMillis now) {
  auto rows = dueRows(db, now, now - kQueueActiveMs);
  std::vector<DueWord> scored;
  scored.reserve(rows.size());
  for (const auto &row : rows) {
    Projection partial;
    partial.word_id = row.word_id;
    partial.strength_days = row.strength_days;
    partial.difficulty = row.difficulty;
    partial.high_priority = row.high_priority;
    partial.first_visit_at = now;
    partial.last_reinforcement_at = row.last_reinforcement_at;
    DueWord due;
    due.word_id = row.word_id;
    due.high_priority = row.high_priority;
    due.next_review_at = row.next_review_at;
    due.retrievability =
        row.last_reinforcement_at ? retrievability(partial, now) : 0;
    scored.push_back(due);
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const DueWord &a, const DueWord &b) {
                     if (a.high_priority != b.high_priority)
                       return a.high_priority && !b.high_priority;
                     return a.retrievability < b.retrievability;
                   });
  return scored;
}
} // namespace lexica::parity::learning
