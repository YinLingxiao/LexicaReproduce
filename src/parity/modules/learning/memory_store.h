#pragma once
#include "parity/modules/learning/scheduler.h"
#include "sqlite3.h"
#include <vector>
namespace lexica::parity::learning {
struct LatestVisit {
  std::int64_t id = 0;
  UtcMillis at = 0;
};
struct EncounterRow {
  std::int64_t id = 0;
  std::int64_t word_id = 0;
  UtcMillis at = 0;
  ComprehensionLevel level = ComprehensionLevel::Unknown;
};
struct DueRow {
  std::int64_t word_id = 0;
  double strength_days = 0;
  double difficulty = 0;
  std::optional<UtcMillis> last_reinforcement_at;
  bool high_priority = false;
  std::optional<UtcMillis> next_review_at;
};
std::int64_t insertVisit(sqlite3 *db, std::int64_t wordId, UtcMillis now);
std::optional<LatestVisit> latestVisit(sqlite3 *db, std::int64_t wordId);
void updateVisitTime(sqlite3 *db, std::int64_t id, UtcMillis now);
void updateLevel(sqlite3 *db, std::int64_t id, ComprehensionLevel level);
std::optional<EncounterRow> getEncounter(sqlite3 *db, std::int64_t id);
std::vector<MemoryEvent> loadEvents(sqlite3 *db, std::int64_t wordId);
void upsertState(sqlite3 *db, const Projection &state, UtcMillis now);
std::optional<Projection> loadProjection(sqlite3 *db, std::int64_t wordId);
std::vector<std::int64_t> wordsWithEvents(sqlite3 *db);
std::vector<DueRow> dueRows(sqlite3 *db, UtcMillis now, UtcMillis activeSince);
} // namespace lexica::parity::learning
