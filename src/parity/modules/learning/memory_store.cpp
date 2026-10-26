#include "parity/modules/learning/memory_store.h"
#include "parity/modules/learning/sql.h"
#include <algorithm>
namespace lexica::parity::learning {
namespace {
std::optional<UtcMillis> parseOptional(const Stmt &stmt, int column) {
  if (stmt.isNull(column))
    return std::nullopt;
  const auto text = stmt.text(column);
  try {
    return parseUtc(text);
  } catch (const Error &) {
    return std::nullopt;
  }
}
} // namespace
std::int64_t insertVisit(sqlite3 *db, std::int64_t wordId, UtcMillis now) {
  Stmt stmt(db, "INSERT INTO encounters(word_id,visited_at,source,comprehension_level) "
                 "VALUES(?1,?2,'lookup','unknown')");
  stmt.bind(1, wordId).bind(2, formatUtc(now));
  stmt.step();
  return sqlite3_last_insert_rowid(db);
}
std::optional<LatestVisit> latestVisit(sqlite3 *db, std::int64_t wordId) {
  Stmt stmt(db, "SELECT id, visited_at FROM encounters WHERE word_id=?1 "
                "ORDER BY id DESC LIMIT 1");
  stmt.bind(1, wordId);
  if (!stmt.step())
    return std::nullopt;
  LatestVisit latest;
  latest.id = stmt.i64(0);
  auto at = parseOptional(stmt, 1);
  if (!at)
    return std::nullopt;
  latest.at = *at;
  return latest;
}
void updateVisitTime(sqlite3 *db, std::int64_t id, UtcMillis now) {
  Stmt stmt(db, "UPDATE encounters SET visited_at=?1 WHERE id=?2");
  stmt.bind(1, formatUtc(now)).bind(2, id).step();
}
void updateLevel(sqlite3 *db, std::int64_t id, ComprehensionLevel level) {
  Stmt stmt(db, "UPDATE encounters SET comprehension_level=?1 WHERE id=?2");
  stmt.bind(1, levelName(level)).bind(2, id).step();
}
std::optional<EncounterRow> getEncounter(sqlite3 *db, std::int64_t id) {
  Stmt stmt(db, "SELECT word_id, visited_at, source, comprehension_level "
                "FROM encounters WHERE id=?1");
  stmt.bind(1, id);
  if (!stmt.step())
    return std::nullopt;
  auto at = parseOptional(stmt, 1);
  auto level = parseLevel(stmt.text(3));
  if (!at || !level || (stmt.text(2) != "lookup" && stmt.text(2) != "review"))
    return std::nullopt;
  EncounterRow row;
  row.id = id;
  row.word_id = stmt.i64(0);
  row.at = *at;
  row.level = *level;
  return row;
}
std::vector<MemoryEvent> loadEvents(sqlite3 *db, std::int64_t wordId) {
  Stmt stmt(db,
            "SELECT kind, at, detail FROM ("
            " SELECT 'visit' AS kind, visited_at AS at, comprehension_level AS detail, id AS seq"
            " FROM encounters WHERE word_id=?1"
            " UNION ALL"
            " SELECT 'review', completed_at, COALESCE(quality, ''), id"
            " FROM reviews WHERE word_id=?1 AND completed_at IS NOT NULL"
            ") ORDER BY at, seq");
  stmt.bind(1, wordId);
  std::vector<MemoryEvent> events;
  while (stmt.step()) {
    const auto kind = stmt.text(0);
    const auto atText = stmt.text(1);
    const auto detail = stmt.text(2);
    UtcMillis at = 0;
    try {
      at = parseUtc(atText);
    } catch (const Error &) {
      throw Error("memory", "corrupt memory event: bad timestamp: " + atText);
    }
    MemoryEvent event;
    event.at = at;
    if (kind == "visit") {
      auto level = parseLevel(detail);
      if (!level)
        throw Error("memory", "corrupt memory event: bad level: " + detail);
      event.level = *level;
    } else if (kind == "review") {
      auto quality = parseQuality(detail);
      if (!quality)
        throw Error("memory", "corrupt memory event: bad quality: " + detail);
      event.review = true;
      event.quality = *quality;
    } else
      throw Error("memory", "corrupt memory event: bad kind: " + kind);
    events.push_back(event);
  }
  return events;
}
void upsertState(sqlite3 *db, const Projection &state, UtcMillis now) {
  Stmt stmt(db,
            "INSERT INTO memory_states"
            "(word_id,strength_days,difficulty,review_count,lapse_count,visit_count,"
            "weak_streak,high_priority,first_visit_at,last_visit_at,last_review_at,"
            "last_reinforcement_at,next_review_at,updated_at)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)"
            " ON CONFLICT(word_id) DO UPDATE SET"
            " strength_days=excluded.strength_days, difficulty=excluded.difficulty,"
            " review_count=excluded.review_count, lapse_count=excluded.lapse_count,"
            " visit_count=excluded.visit_count, weak_streak=excluded.weak_streak,"
            " high_priority=excluded.high_priority, first_visit_at=excluded.first_visit_at,"
            " last_visit_at=excluded.last_visit_at, last_review_at=excluded.last_review_at,"
            " last_reinforcement_at=excluded.last_reinforcement_at,"
            " next_review_at=excluded.next_review_at, updated_at=excluded.updated_at");
  auto textOf = [](const std::optional<UtcMillis> &value) -> std::optional<std::string> {
    if (!value)
      return std::nullopt;
    return formatUtc(*value);
  };
  stmt.bind(1, state.word_id);
  stmt.bind(2, state.strength_days);
  stmt.bind(3, state.difficulty);
  stmt.bind(4, state.review_count);
  stmt.bind(5, state.lapse_count);
  stmt.bind(6, state.visit_count);
  stmt.bind(7, state.weak_streak);
  stmt.bind(8, static_cast<std::int64_t>(state.high_priority ? 1 : 0));
  stmt.bind(9, formatUtc(state.first_visit_at));
  stmt.bind(10, textOf(state.last_visit_at));
  stmt.bind(11, textOf(state.last_review_at));
  stmt.bind(12, textOf(state.last_reinforcement_at));
  stmt.bind(13, textOf(state.next_review_at));
  stmt.bind(14, formatUtc(now));
  stmt.step();
}
std::optional<Projection> loadProjection(sqlite3 *db, std::int64_t wordId) {
  Stmt stmt(db, "SELECT strength_days,difficulty,review_count,lapse_count,visit_count,"
                "weak_streak,high_priority,first_visit_at,last_visit_at,last_review_at,"
                "last_reinforcement_at,next_review_at FROM memory_states WHERE word_id=?1");
  stmt.bind(1, wordId);
  if (!stmt.step())
    return std::nullopt;
  auto first = parseOptional(stmt, 7);
  if (!first)
    return std::nullopt;
  Projection state;
  state.word_id = wordId;
  state.strength_days = stmt.f64(0);
  state.difficulty = stmt.f64(1);
  state.review_count = std::max<std::int64_t>(stmt.i64(2), 0);
  state.lapse_count = std::max<std::int64_t>(stmt.i64(3), 0);
  state.visit_count = std::max<std::int64_t>(stmt.i64(4), 0);
  state.weak_streak = std::max<std::int64_t>(stmt.i64(5), 0);
  state.high_priority = stmt.i64(6) != 0;
  state.first_visit_at = *first;
  state.last_visit_at = parseOptional(stmt, 8);
  state.last_review_at = parseOptional(stmt, 9);
  state.last_reinforcement_at = parseOptional(stmt, 10);
  state.next_review_at = parseOptional(stmt, 11);
  return state;
}
std::vector<std::int64_t> wordsWithEvents(sqlite3 *db) {
  Stmt stmt(db, "SELECT DISTINCT word_id FROM encounters");
  std::vector<std::int64_t> ids;
  while (stmt.step())
    ids.push_back(stmt.i64(0));
  return ids;
}
std::vector<DueRow> dueRows(sqlite3 *db, UtcMillis now, UtcMillis activeSince) {
  Stmt stmt(db,
            "SELECT m.word_id,m.strength_days,m.difficulty,m.last_reinforcement_at,"
            "m.high_priority,m.next_review_at FROM memory_states m "
            "WHERE m.next_review_at IS NOT NULL AND m.next_review_at<=?1 "
            "AND m.word_id NOT IN (SELECT word_id FROM ignored_words) "
            "AND ((m.last_visit_at IS NOT NULL AND m.last_visit_at>=?2) "
            "OR (m.last_review_at IS NOT NULL AND m.last_review_at>=?2))");
  stmt.bind(1, formatUtc(now)).bind(2, formatUtc(activeSince));
  std::vector<DueRow> rows;
  while (stmt.step()) {
    DueRow row;
    row.word_id = stmt.i64(0);
    row.strength_days = stmt.f64(1);
    row.difficulty = stmt.f64(2);
    row.last_reinforcement_at = parseOptional(stmt, 3);
    row.high_priority = stmt.i64(4) != 0;
    row.next_review_at = parseOptional(stmt, 5);
    rows.push_back(row);
  }
  return rows;
}
} // namespace lexica::parity::learning
