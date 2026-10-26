#include "parity/modules/modules.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
namespace lexica::parity {
namespace {
constexpr double kRetention = 0.90;
constexpr std::int64_t kDayMs = 86400000;
constexpr std::int64_t kActiveWindowMs = 60 * kDayMs;

struct CivilDate {
  int year;
  unsigned month;
  unsigned day;
};

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  const auto q = a / b;
  const auto r = a % b;
  return r != 0 && ((r < 0) != (b < 0)) ? q - 1 : q;
}

CivilDate civilFromDays(int z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2), m, d};
}

std::string formatDate(CivilDate date) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", date.year,
                date.month, date.day);
  return buffer;
}

bool isSpace(char32_t cp) {
  switch (cp) {
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x20:
  case 0x85:
  case 0xA0:
  case 0x1680:
  case 0x2028:
  case 0x2029:
  case 0x202F:
  case 0x205F:
  case 0x3000:
    return true;
  default:
    return cp >= 0x2000 && cp <= 0x200A;
  }
}

bool nextCp(const std::string &text, std::size_t &index, char32_t &cp) {
  if (index >= text.size())
    return false;
  const auto c = static_cast<unsigned char>(text[index]);
  std::size_t len = 1;
  char32_t value = c;
  if (c < 0x80) {
    value = c;
  } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
    len = 2;
    value = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    value = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
    len = 4;
    value = c & 0x07;
  } else {
    return false;
  }
  if (index + len > text.size())
    return false;
  for (std::size_t k = 1; k < len; ++k) {
    const auto cc = static_cast<unsigned char>(text[index + k]);
    if ((cc & 0xC0) != 0x80)
      return false;
    value = (value << 6) | (cc & 0x3F);
  }
  if ((len == 3 && value < 0x800) || (len == 4 && value < 0x10000) ||
      value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
    return false;
  index += len;
  cp = value;
  return true;
}

std::size_t codePoints(const std::string &text) {
  std::size_t count = 0;
  std::size_t index = 0;
  char32_t cp = 0;
  while (index < text.size()) {
    if (!nextCp(text, index, cp))
      throw Error("BAD_REQUEST", "Invalid text encoding");
    ++count;
  }
  return count;
}

std::string trimText(const std::string &text) {
  std::size_t index = 0;
  std::size_t start = 0;
  std::size_t end = 0;
  bool started = false;
  char32_t cp = 0;
  while (index < text.size()) {
    const auto at = index;
    if (!nextCp(text, index, cp))
      throw Error("BAD_REQUEST", "Invalid text encoding");
    if (!isSpace(cp)) {
      if (!started) {
        start = at;
        started = true;
      }
      end = index;
    }
  }
  return started ? text.substr(start, end - start) : std::string{};
}

std::optional<std::string> maybeText(const Statement &row, int column) {
  if (row.isNull(column))
    return std::nullopt;
  return row.text(column);
}

std::int64_t limitOr(const std::optional<std::int64_t> &limit,
                     std::int64_t fallback) {
  const auto value = limit.value_or(fallback);
  if (value < 0 || value > 255)
    throw Error("BAD_REQUEST", "limit is outside 0..255");
  return value;
}

double parseReal(const std::string &text) {
  double value = 0;
  const auto end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end)
    throw Error("DB_ERROR", "Invalid strength_days");
  return value;
}

double retrievability(double strength, const std::optional<std::string> &anchor,
                      UtcMillis now) {
  if (!anchor)
    return 0;
  const auto elapsed = now - parseUtc(*anchor);
  const auto days = static_cast<double>(elapsed) / static_cast<double>(kDayMs);
  if (days <= 0)
    return 1;
  const auto span =
      strength > 0 ? strength : std::numeric_limits<double>::min();
  return std::pow(kRetention, days / span);
}

ComprehensionLevel comprehension(const std::string &value) {
  if (value == "context")
    return ComprehensionLevel::Context;
  if (value == "english")
    return ComprehensionLevel::English;
  if (value == "chinese")
    return ComprehensionLevel::Chinese;
  if (value == "unknown")
    return ComprehensionLevel::Unknown;
  throw Error("DB_ERROR", "Invalid comprehension_level");
}

const char *kVocabulary = R"SQL(
WITH ids AS (
  SELECT word_id FROM memory_states
  UNION
  SELECT word_id FROM word_notes WHERE bookmarked=1 OR note<>''
), vocabulary AS (
  SELECT w.id, w.word, w.display, w.phonetic,
         COALESCE(n.bookmarked, 0) AS bookmarked,
         COALESCE(n.note, '') AS note,
         COALESCE(m.visit_count, 0) AS visit_count,
         m.next_review_at,
         COALESCE(m.last_visit_at, n.updated_at, '') AS touched,
         CASE WHEN i.word_id IS NOT NULL THEN 'ignored'
              WHEN m.word_id IS NULL THEN 'new'
              WHEN m.strength_days < 4.0 THEN 'learning'
              WHEN m.strength_days < 21.0 THEN 'familiar'
              ELSE 'stable' END AS status
  FROM ids
  JOIN words w ON w.id = ids.word_id
  LEFT JOIN memory_states m ON m.word_id = w.id
  LEFT JOIN word_notes n ON n.word_id = w.id
  LEFT JOIN ignored_words i ON i.word_id = w.id
), filtered AS (
  SELECT * FROM vocabulary
  WHERE (instr(lower(word), lower(?1)) > 0 OR instr(lower(note), lower(?1)) > 0)
    AND (?2 = 'all' OR (?2 = 'bookmarked' AND bookmarked = 1) OR status = ?2)
)
)SQL";

WordNote readNote(sqlite3 *db, std::int64_t wordId) {
  Statement query(db,
                  "SELECT COALESCE(n.bookmarked, 0), COALESCE(n.note, ''), "
                  "i.word_id IS NOT NULL FROM words w "
                  "LEFT JOIN word_notes n ON n.word_id = w.id "
                  "LEFT JOIN ignored_words i ON i.word_id = w.id WHERE w.id=?");
  query.bind(1, wordId);
  if (!query.step())
    throw Error("DB_ERROR", "no such word");
  return WordNote{query.integer(0) != 0, query.text(1), query.integer(2) != 0};
}

void saveNote(sqlite3 *db, std::int64_t wordId, bool bookmarked,
              const std::string &note, UtcMillis now) {
  if (codePoints(note) > 10000)
    throw Error("BAD_REQUEST", "A note may not exceed 10,000 characters");
  Transaction tx(db);
  {
    Statement write(
        db, "INSERT INTO word_notes(word_id, bookmarked, note, updated_at) "
            "VALUES(?,?,?,?) ON CONFLICT(word_id) DO UPDATE SET "
            "bookmarked=excluded.bookmarked, note=excluded.note, "
            "updated_at=excluded.updated_at");
    write.bind(1, wordId)
        .bind(2, bookmarked ? std::int64_t{1} : std::int64_t{0})
        .bind(3, note)
        .bind(4, formatUtc(now))
        .step();
  }
  tx.commit();
}

void setIgnored(sqlite3 *db, std::int64_t wordId, bool ignored) {
  Transaction tx(db);
  if (ignored) {
    {
      Statement exists(db, "SELECT 1 FROM words WHERE id=?");
      exists.bind(1, wordId);
      if (!exists.step())
        throw Error("DB_ERROR", "no such word");
    }
    Statement insert(db,
                     "INSERT OR IGNORE INTO ignored_words(word_id) VALUES(?)");
    insert.bind(1, wordId).step();
  } else {
    Statement remove(db, "DELETE FROM ignored_words WHERE word_id=?");
    remove.bind(1, wordId).step();
  }
  tx.commit();
}

LibraryPage listWords(sqlite3 *db, const std::string &query,
                      const std::string &filter, std::int64_t offset) {
  const auto needle = trimText(query);
  Statement count(db, std::string(kVocabulary) + " SELECT COUNT(*) FROM filtered");
  count.bind(1, needle).bind(2, filter);
  if (!count.step())
    throw Error("DB_ERROR", "library count failed");
  LibraryPage page;
  page.total = count.integer(0);
  Statement rows(
      db, std::string(kVocabulary) +
              " SELECT f.id, f.word, f.display, f.phonetic, f.status, "
              "f.bookmarked, f.note, f.visit_count, f.next_review_at, "
              "COALESCE((SELECT COALESCE(NULLIF(chinese_definition, ''), "
              "english_definition) FROM senses WHERE word_id=f.id "
              "ORDER BY order_idx, id LIMIT 1), 'No definition') "
              "FROM filtered f ORDER BY touched DESC, id DESC LIMIT 24 OFFSET ?3");
  rows.bind(1, needle).bind(2, filter).bind(3, offset);
  while (rows.step()) {
    LibraryWord word;
    word.id = rows.integer(0);
    word.word = rows.text(1);
    word.display = rows.text(2);
    word.phonetic = maybeText(rows, 3);
    word.status = rows.text(4);
    word.bookmarked = rows.integer(5) != 0;
    word.note = rows.text(6);
    word.visit_count = rows.integer(7);
    word.next_review_at = maybeText(rows, 8);
    word.definition = rows.text(9);
    page.words.push_back(std::move(word));
  }
  return page;
}

std::vector<RecentWord> recentWords(sqlite3 *db, std::int64_t limit) {
  Statement query(db,
                  "SELECT w.word, w.display, w.phonetic, e.visited_at, "
                  "e.comprehension_level FROM encounters e "
                  "JOIN (SELECT word_id, MAX(id) AS max_id FROM encounters "
                  "GROUP BY word_id) t ON t.max_id = e.id "
                  "JOIN words w ON w.id = e.word_id "
                  "ORDER BY e.visited_at DESC LIMIT ?");
  query.bind(1, limit);
  std::vector<RecentWord> words;
  while (query.step()) {
    RecentWord word;
    word.word = query.text(0);
    word.display = query.text(1);
    word.phonetic = maybeText(query, 2);
    word.visited_at = query.text(3);
    word.comprehension_level = comprehension(query.text(4));
    words.push_back(std::move(word));
  }
  return words;
}

StatsSummary summarize(sqlite3 *db, UtcMillis now) {
  const auto current = formatUtc(now);
  const auto active = formatUtc(now - kActiveWindowMs);
  Statement query(
      db,
      "SELECT COUNT(*), "
      "COALESCE(SUM(CASE WHEN i.word_id IS NOT NULL THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN i.word_id IS NULL AND m.strength_days < 4.0 "
      "THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN i.word_id IS NULL AND m.strength_days >= 4.0 "
      "AND m.strength_days < 21.0 THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN i.word_id IS NULL AND m.strength_days >= 21.0 "
      "THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN i.word_id IS NULL AND m.next_review_at IS NOT NULL "
      "AND m.next_review_at <= ?1 AND ((m.last_visit_at IS NOT NULL AND "
      "m.last_visit_at >= ?2) OR (m.last_review_at IS NOT NULL AND "
      "m.last_review_at >= ?2)) THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN i.word_id IS NULL AND m.high_priority != 0 "
      "THEN 1 ELSE 0 END), 0) "
      "FROM memory_states m LEFT JOIN ignored_words i ON i.word_id = m.word_id");
  query.bind(1, current).bind(2, active);
  if (!query.step())
    throw Error("DB_ERROR", "stats query failed");
  return StatsSummary{query.integer(0), query.integer(2), query.integer(3),
                      query.integer(4), query.integer(1), query.integer(5),
                      query.integer(6)};
}

std::vector<ActivityDay> activityDays(sqlite3 *db, UtcMillis now,
                                      std::int64_t offsetMinutes) {
  const auto offset = std::clamp(offsetMinutes, std::int64_t{-840},
                                 std::int64_t{840});
  const auto today = floorDiv(now + offset * 60000, kDayMs);
  const auto since = formatUtc(now - 15 * kDayMs);
  const auto shift = std::to_string(offset) + " minutes";
  Statement query(
      db,
      "SELECT day, SUM(lookups), SUM(reviews) FROM ("
      "SELECT date(visited_at, ?1) AS day, COUNT(*) AS lookups, 0 AS reviews "
      "FROM encounters WHERE visited_at >= ?2 GROUP BY day "
      "UNION ALL "
      "SELECT date(completed_at, ?1) AS day, 0, COUNT(*) FROM reviews "
      "WHERE completed_at >= ?2 GROUP BY day) GROUP BY day");
  query.bind(1, shift).bind(2, since);
  std::map<std::string, std::pair<std::int64_t, std::int64_t>> counts;
  while (query.step()) {
    if (!query.isNull(0))
      counts[query.text(0)] = {query.integer(1), query.integer(2)};
  }
  std::vector<ActivityDay> days;
  days.reserve(14);
  for (int i = 0; i < 14; ++i) {
    const auto date =
        formatDate(civilFromDays(static_cast<int>(today - 13 + i)));
    const auto found = counts.find(date);
    days.push_back(ActivityDay{
        date, found == counts.end() ? 0 : found->second.first,
        found == counts.end() ? 0 : found->second.second});
  }
  return days;
}

std::vector<FadingWord> fadingWords(sqlite3 *db, UtcMillis now,
                                    std::int64_t limit) {
  const auto current = formatUtc(now);
  const auto active = formatUtc(now - kActiveWindowMs);
  Statement query(
      db,
      "SELECT w.word, w.display, m.high_priority, m.next_review_at, "
      "printf('%.17g', m.strength_days), m.last_reinforcement_at, m.word_id "
      "FROM memory_states m JOIN words w ON w.id = m.word_id "
      "WHERE m.next_review_at IS NOT NULL AND m.next_review_at <= ?1 "
      "AND m.word_id NOT IN (SELECT word_id FROM ignored_words) "
      "AND ((m.last_visit_at IS NOT NULL AND m.last_visit_at >= ?2) "
      "OR (m.last_review_at IS NOT NULL AND m.last_review_at >= ?2)) "
      "ORDER BY m.word_id");
  query.bind(1, current).bind(2, active);
  struct Due {
    FadingWord word;
    bool highPriority;
    double r;
    std::int64_t wordId;
  };
  std::vector<Due> due;
  while (query.step()) {
    Due row;
    row.word.word = query.text(0);
    row.word.display = query.text(1);
    row.highPriority = query.integer(2) != 0;
    row.word.high_priority = row.highPriority;
    row.word.next_review_at = maybeText(query, 3);
    row.r = retrievability(parseReal(query.text(4)), maybeText(query, 5), now);
    row.wordId = query.integer(6);
    due.push_back(std::move(row));
  }
  std::sort(due.begin(), due.end(), [](const Due &a, const Due &b) {
    if (a.highPriority != b.highPriority)
      return a.highPriority && !b.highPriority;
    if (a.r < b.r)
      return true;
    if (b.r < a.r)
      return false;
    return a.wordId < b.wordId;
  });
  if (static_cast<std::int64_t>(due.size()) > limit)
    due.resize(static_cast<std::size_t>(limit));
  std::vector<FadingWord> words;
  words.reserve(due.size());
  for (auto &row : due)
    words.push_back(std::move(row.word));
  return words;
}
} // namespace

void registerLibrary(Router &router) {
  router.bind("library", "word_note", [](const Json &args, Context &context) {
    const auto input = args.get<WordNoteArgs>();
    return context.db.withConnection(
        [&](sqlite3 *db) { return readNote(db, input.wordId); });
  });
  router.bind("library", "save_word_note",
              [](const Json &args, Context &context) {
                const auto input = args.get<SaveWordNoteArgs>();
                const auto now = context.clock.nowMillis();
                context.db.withConnection([&](sqlite3 *db) {
                  saveNote(db, input.wordId, input.bookmarked, input.note, now);
                });
                return Json(nullptr);
              });
  router.bind("library", "set_word_ignored",
              [](const Json &args, Context &context) {
                const auto input = args.get<SetWordIgnoredArgs>();
                context.db.withConnection([&](sqlite3 *db) {
                  setIgnored(db, input.wordId, input.ignored);
                });
                return Json(nullptr);
              });
  router.bind("library", "library_words",
              [](const Json &args, Context &context) {
                const auto input = args.get<LibraryWordsArgs>();
                return context.db.withConnection([&](sqlite3 *db) {
                  return listWords(db, input.query, input.filter, input.offset);
                });
              });
  router.bind("library", "recent_words", [](const Json &args, Context &context) {
    const auto input = args.get<RecentWordsArgs>();
    const auto limit = limitOr(input.limit, 6);
    return context.db.withConnection(
        [&](sqlite3 *db) { return recentWords(db, limit); });
  });
  router.bind("library", "stats_summary", [](const Json &, Context &context) {
    const auto now = context.clock.nowMillis();
    return context.db.withConnection(
        [&](sqlite3 *db) { return summarize(db, now); });
  });
  router.bind("library", "activity_days",
              [](const Json &args, Context &context) {
                const auto input = args.get<ActivityDaysArgs>();
                const auto now = context.clock.nowMillis();
                return context.db.withConnection([&](sqlite3 *db) {
                  return activityDays(db, now, input.offsetMinutes);
                });
              });
  router.bind("library", "fading_words", [](const Json &args, Context &context) {
    const auto input = args.get<FadingWordsArgs>();
    const auto limit = limitOr(input.limit, 20);
    const auto now = context.clock.nowMillis();
    return context.db.withConnection(
        [&](sqlite3 *db) { return fadingWords(db, now, limit); });
  });
}
} // namespace lexica::parity
