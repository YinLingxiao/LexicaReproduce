#include "parity/modules/learning/review_service.h"
#include "parity/modules/learning/memory_service.h"
#include "parity/modules/learning/sql.h"
#include "parity/storage/database.h"
#include <algorithm>
#include <ctime>
namespace lexica::parity::learning {
namespace {
void ensureStudySchema(sqlite3 *db) {
  execute(db,
          "CREATE TABLE IF NOT EXISTS custom_wordbook_words ("
          " word_id INTEGER PRIMARY KEY REFERENCES words(id) ON DELETE CASCADE,"
          " added_at TEXT NOT NULL DEFAULT '');"
          "CREATE TABLE IF NOT EXISTS word_enrichments ("
          " word_id INTEGER PRIMARY KEY REFERENCES words(id) ON DELETE CASCADE,"
          " mnemonic TEXT, roots_json TEXT NOT NULL DEFAULT '[]',"
          " exam_example_text TEXT, exam_example_translation TEXT,"
          " standard_example_text TEXT, standard_example_translation TEXT,"
          " updated_at TEXT NOT NULL);");
}
bool columnExists(sqlite3 *db, const char *table, const char *column) {
  Stmt stmt(db, std::string("PRAGMA table_info(") + table + ")");
  while (stmt.step())
    if (stmt.text(1) == column)
      return true;
  return false;
}
std::string wordbookFilter(sqlite3 *db, const std::string &wordbook) {
  if (wordbook == "custom")
    return "w.id IN (SELECT word_id FROM custom_wordbook_words UNION "
           "SELECT word_id FROM word_notes WHERE bookmarked = 1)";
  if (wordbook == "encountered")
    return "EXISTS (SELECT 1 FROM encounters e WHERE e.word_id = w.id)";
  const char *known[] = {"cet4", "cet6", "ky", "toefl", "ielts", "gre", "gk", "oxford"};
  if (std::none_of(std::begin(known), std::end(known),
                   [&](const char *id) { return wordbook == id; }))
    throw Error("review", "word data unavailable for review");
  const auto tags = columnExists(db, "words", "tags")
                        ? std::string("instr(lower(w.tags), ?1) > 0")
                        : std::string("0");
  auto filter = wordbook == "oxford" && columnExists(db, "words", "oxford")
                    ? "(w.oxford = 1 OR " + tags + ")"
                    : tags;
  filter = "(" + filter + ") AND ?1 IS NOT NULL";
  Stmt exists(db, "SELECT EXISTS(SELECT 1 FROM words w WHERE " + filter + ")");
  exists.bind(1, wordbook);
  exists.step();
  if (exists.i64(0))
    return filter;
  return "(w.source = 'seed' OR w.frequency_rank > 0) AND ?1 IS NOT NULL";
}
UtcMillis utcMidnight(UtcMillis now) {
  auto seconds = now / 1000;
  auto millis = static_cast<int>(now % 1000);
  if (millis < 0) {
    millis += 1000;
    --seconds;
  }
  std::tm tm{};
  const auto clock = static_cast<std::time_t>(seconds);
  if (gmtime_s(&tm, &clock))
    throw Error("BAD_REQUEST", "Timestamp is out of range");
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  return _mkgmtime64(&tm) * 1000;
}
const Sense *findSense(const WordEntry &entry, std::optional<std::int64_t> senseId) {
  if (!senseId)
    return entry.senses.empty() ? nullptr : &entry.senses.front();
  for (const auto &sense : entry.senses)
    if (sense.id == *senseId)
      return &sense;
  return nullptr;
}
Hint hintFor(const WordEntry &entry, std::optional<std::int64_t> senseId,
             std::int64_t hintNo) {
  const auto *sense = findSense(entry, senseId);
  if (!sense)
    throw Error("review", "word data unavailable for review");
  if (hintNo == 1)
    return Hint{HintKind::Mask, maskHint(entry.display)};
  if (hintNo == 2)
    return Hint{HintKind::English, sense->english_definition};
  return Hint{HintKind::Chinese, sense->chinese_definition.value_or("")};
}
std::optional<StudyCardEntry> studyCard(sqlite3 *db, DictionaryPort &dictionary,
                                        std::int64_t wordId, bool isNew) {
  auto entry = dictionary.byId(db, wordId);
  if (!entry)
    return std::nullopt;
  auto prompt = buildDefinitionPrompt(*entry);
  if (entry->senses.empty() || isBlank(prompt.text))
    return std::nullopt;
  Stmt flags(db, "SELECT EXISTS(SELECT 1 FROM word_notes WHERE word_id=?1 AND bookmarked=1),"
                 " EXISTS(SELECT 1 FROM custom_wordbook_words WHERE word_id=?1)");
  flags.bind(1, wordId);
  flags.step();
  StudyCardEntry card;
  card.entry = std::move(*entry);
  card.item.word_id = wordId;
  card.item.prompt = std::move(prompt.text);
  card.item.kind = prompt.kind;
  card.item.source = PromptSource::Dictionary;
  card.item.sense_id = prompt.sense_id;
  card.item.example_id = prompt.example_id;
  card.is_new = isNew;
  card.bookmarked = flags.i64(0) != 0;
  card.in_custom_wordbook = flags.i64(1) != 0;
  return card;
}
struct PendingReview {
  std::int64_t word_id = 0;
  std::optional<std::int64_t> sense_id;
  std::int64_t hints_used = 0;
};
PendingReview loadPending(sqlite3 *db, std::int64_t reviewId) {
  Stmt stmt(db, "SELECT id, word_id, sense_id, hints_used, completed_at "
                "FROM reviews WHERE id=?1");
  stmt.bind(1, reviewId);
  if (!stmt.step())
    throw Error("review", "review " + std::to_string(reviewId) + " not found");
  if (!stmt.isNull(4))
    throw Error("review",
                "review " + std::to_string(stmt.i64(0)) + " already completed");
  PendingReview pending;
  pending.word_id = stmt.i64(1);
  if (!stmt.isNull(2))
    pending.sense_id = stmt.i64(2);
  pending.hints_used = std::max<std::int64_t>(stmt.i64(3), 0);
  return pending;
}
} // namespace
StudySessionGroup studyGroup(sqlite3 *db, DictionaryPort &dictionary,
                             const std::string &wordbook, std::int64_t dailyNewLimit,
                             StudyPriority priority, UtcMillis now) {
  ensureStudySchema(db);
  const auto filter = wordbookFilter(db, wordbook);
  const auto midnight = utcMidnight(now);
  Stmt learned(db, "SELECT COUNT(*) FROM memory_states WHERE first_visit_at>=?1 "
                   "AND first_visit_at<?2");
  learned.bind(1, formatUtc(midnight))
      .bind(2, formatUtc(midnight + kDayMs))
      .step();
  const auto today = learned.i64(0);
  auto due = dueWords(db, now);
  const auto dueCount = static_cast<std::int64_t>(due.size());
  std::vector<StudyCardEntry> dueCards;
  for (const auto &word : due) {
    if (auto card = studyCard(db, dictionary, word.word_id, false)) {
      dueCards.push_back(std::move(*card));
      if (dueCards.size() == static_cast<std::size_t>(kStudyLimit))
        break;
    }
  }
  std::vector<StudyCardEntry> newCards;
  const auto remaining = std::max<std::int64_t>(dailyNewLimit - today, 0);
  const auto quota = std::min<std::int64_t>(remaining, kStudyLimit);
  if (wordbook != "encountered" && quota > 0) {
    Stmt ids(db, "SELECT w.id FROM words w WHERE (" + filter +
                     ") AND ?1 IS NOT NULL"
                     " AND NOT EXISTS (SELECT 1 FROM memory_states m WHERE m.word_id=w.id)"
                     " AND NOT EXISTS (SELECT 1 FROM ignored_words i WHERE i.word_id=w.id)"
                     " AND EXISTS (SELECT 1 FROM senses s WHERE s.word_id=w.id"
                     " AND (trim(s.english_definition)<>'' OR trim(coalesce(s.chinese_definition,''))<>''))"
                     " ORDER BY CASE WHEN w.frequency_rank>0 THEN 0 ELSE 1 END,"
                     " w.frequency_rank, w.id LIMIT ?2");
    ids.bind(1, wordbook).bind(2, quota);
    while (ids.step()) {
      if (auto card = studyCard(db, dictionary, ids.i64(0), true))
        newCards.push_back(std::move(*card));
    }
  }
  const auto newCount = static_cast<std::int64_t>(newCards.size());
  std::vector<StudyCardEntry> cards;
  if (priority == StudyPriority::NewFirst) {
    cards = std::move(newCards);
    cards.insert(cards.end(), std::make_move_iterator(dueCards.begin()),
                 std::make_move_iterator(dueCards.end()));
    if (cards.size() > static_cast<std::size_t>(kStudyLimit))
      cards.resize(kStudyLimit);
  } else if (priority == StudyPriority::ReviewFirst) {
    cards = std::move(dueCards);
    cards.insert(cards.end(), std::make_move_iterator(newCards.begin()),
                 std::make_move_iterator(newCards.end()));
    if (cards.size() > static_cast<std::size_t>(kStudyLimit))
      cards.resize(kStudyLimit);
  } else {
    auto dueIt = dueCards.begin();
    auto newIt = newCards.begin();
    while (cards.size() < static_cast<std::size_t>(kStudyLimit)) {
      if (dueIt == dueCards.end() && newIt == newCards.end())
        break;
      std::optional<StudyCardEntry> dueCard;
      std::optional<StudyCardEntry> newCard;
      if (dueIt != dueCards.end())
        dueCard = std::move(*dueIt++);
      if (newIt != newCards.end())
        newCard = std::move(*newIt++);
      if (dueCard)
        cards.push_back(std::move(*dueCard));
      if (newCard && cards.size() < static_cast<std::size_t>(kStudyLimit))
        cards.push_back(std::move(*newCard));
    }
  }
  return StudySessionGroup{std::move(cards), dueCount, newCount, today};
}
ReviewFeedback studyGrade(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                          StudyGrade grade, UtcMillis now) {
  Transaction tx(db);
  auto entry = dictionary.byId(db, wordId);
  if (!entry)
    throw Error("review", "word data unavailable for review");
  Stmt encountered(db, "SELECT EXISTS(SELECT 1 FROM encounters WHERE word_id=?1)");
  encountered.bind(1, wordId).step();
  if (!encountered.i64(0))
    openVisit(db, wordId, now);
  const auto quality = grade == StudyGrade::Know    ? RecallQuality::Good
                       : grade == StudyGrade::Hazy   ? RecallQuality::Hard
                                                     : RecallQuality::Forgotten;
  const auto correct = grade != StudyGrade::Forgot;
  Stmt review(db, "INSERT INTO reviews(word_id,started_at,completed_at,quality) "
                  "VALUES(?1,?2,?2,?3)");
  review.bind(1, wordId).bind(2, formatUtc(now)).bind(3, qualityName(quality)).step();
  const auto reviewId = sqlite3_last_insert_rowid(db);
  Stmt attempt(db, "INSERT INTO review_attempts(review_id,at,action,correct) "
                   "VALUES(?1,?2,'answer',?3)");
  attempt.bind(1, reviewId)
      .bind(2, formatUtc(now))
      .bind(3, static_cast<std::int64_t>(correct ? 1 : 0))
      .step();
  auto memory = refreshWord(db, wordId, now);
  tx.commit();
  ReviewFeedback feedback;
  feedback.correct = correct;
  feedback.quality = quality;
  feedback.answer = entry->display;
  feedback.hints_used = 0;
  if (memory)
    feedback.memory = toDto(*memory);
  return feedback;
}
std::vector<WordbookInfo> wordbooksList(sqlite3 *db) {
  ensureStudySchema(db);
  const std::pair<const char *, const char *> books[] = {
      {"cet4", "CET-4"},     {"cet6", "CET-6"},
      {"ky", "考研"},        {"toefl", "TOEFL"},
      {"ielts", "IELTS"},    {"gre", "GRE"},
      {"gk", "高考"},        {"oxford", "Oxford 3000"},
      {"custom", "自定义单词本"}};
  std::vector<WordbookInfo> result;
  for (const auto &book : books) {
    const auto filter = wordbookFilter(db, book.first);
    Stmt count(db, "SELECT COUNT(*), COUNT(m.word_id) FROM words w "
                   "LEFT JOIN memory_states m ON m.word_id=w.id WHERE (" +
                       filter + ") AND ?1 IS NOT NULL");
    count.bind(1, book.first).step();
    result.push_back(WordbookInfo{book.first, book.second, count.i64(0), count.i64(1)});
  }
  return result;
}
void customWordbookToggle(sqlite3 *db, std::int64_t wordId, bool added) {
  ensureStudySchema(db);
  Transaction tx(db);
  if (added) {
    Stmt stmt(db, "INSERT OR IGNORE INTO custom_wordbook_words(word_id) VALUES(?1)");
    stmt.bind(1, wordId).step();
  } else {
    Stmt stmt(db, "DELETE FROM custom_wordbook_words WHERE word_id=?1");
    stmt.bind(1, wordId).step();
  }
  tx.commit();
}
std::vector<ReviewGroupEntry> reviewGroup(sqlite3 *db, DictionaryPort &dictionary,
                                          UtcMillis now) {
  std::vector<ReviewGroupEntry> items;
  for (const auto &due : dueWords(db, now)) {
    auto entry = dictionary.byId(db, due.word_id);
    if (!entry || entry->senses.empty())
      continue;
    auto prompt = buildDefinitionPrompt(*entry);
    if (isBlank(prompt.text))
      continue;
    ReviewGroupEntry group;
    group.item.word_id = due.word_id;
    group.item.prompt = std::move(prompt.text);
    group.item.kind = prompt.kind;
    group.item.source = PromptSource::Dictionary;
    group.item.sense_id = prompt.sense_id;
    group.item.example_id = prompt.example_id;
    group.entry = std::move(*entry);
    items.push_back(std::move(group));
    if (items.size() >= static_cast<std::size_t>(kReviewLimit))
      break;
  }
  return items;
}
std::vector<ReviewItem> reviewQueue(sqlite3 *db, DictionaryPort &dictionary,
                                    UtcMillis now) {
  std::vector<ReviewItem> items;
  for (auto &group : reviewGroup(db, dictionary, now))
    items.push_back(std::move(group.item));
  return items;
}
bool practiceCheck(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                   const std::optional<std::string> &answer) {
  auto entry = dictionary.byId(db, wordId);
  if (!entry)
    throw Error("review", "word data unavailable for review");
  return answer && isCorrect(*answer, entry->word);
}
Hint practiceHint(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                  const std::optional<std::int64_t> &senseId, std::int64_t hintNo) {
  if (hintNo < 1 || hintNo > 3)
    throw Error("review", "invalid hint number " + std::to_string(hintNo));
  auto entry = dictionary.byId(db, wordId);
  if (!entry)
    throw Error("review", "word data unavailable for review");
  return hintFor(*entry, senseId, hintNo);
}
std::int64_t startReview(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                         const std::optional<std::int64_t> &senseId,
                         const std::optional<std::int64_t> &exampleId, UtcMillis now) {
  Transaction tx(db);
  auto entry = dictionary.byId(db, wordId);
  if (!entry)
    throw Error("review", "word data unavailable for review");
  const Sense *selected = nullptr;
  if (senseId) {
    selected = findSense(*entry, senseId);
    if (!selected || selected->id != *senseId)
      throw Error("review", "word data unavailable for review");
  }
  if (exampleId) {
    bool valid = false;
    if (selected) {
      for (const auto &example : selected->examples)
        valid = valid || example.id == *exampleId;
    } else {
      for (const auto &sense : entry->senses)
        for (const auto &example : sense.examples)
          valid = valid || example.id == *exampleId;
    }
    if (!valid)
      throw Error("review", "word data unavailable for review");
  }
  Stmt stmt(db, "INSERT INTO reviews(word_id,started_at,sense_id,example_id) "
                "VALUES(?1,?2,?3,?4)");
  stmt.bind(1, wordId).bind(2, formatUtc(now)).bind(3, senseId).bind(4, exampleId).step();
  const auto id = sqlite3_last_insert_rowid(db);
  tx.commit();
  return id;
}
Hint requestHint(sqlite3 *db, DictionaryPort &dictionary, std::int64_t reviewId,
                 std::int64_t hintNo, UtcMillis now) {
  if (hintNo < 1 || hintNo > 3)
    throw Error("review", "invalid hint number " + std::to_string(hintNo));
  Transaction tx(db);
  auto pending = loadPending(db, reviewId);
  const auto expected = pending.hints_used + 1;
  if (hintNo != expected)
    throw Error("review", "expected hint " + std::to_string(expected) + ", got " +
                               std::to_string(hintNo));
  auto entry = dictionary.byId(db, pending.word_id);
  if (!entry)
    throw Error("review", "word data unavailable for review");
  auto hint = hintFor(*entry, pending.sense_id, hintNo);
  const char *action = hintNo == 1 ? "hint1" : hintNo == 2 ? "hint2" : "hint3";
  Stmt attempt(db, "INSERT INTO review_attempts(review_id,at,action) VALUES(?1,?2,?3)");
  attempt.bind(1, reviewId).bind(2, formatUtc(now)).bind(3, action).step();
  Stmt update(db, "UPDATE reviews SET hints_used=MAX(hints_used,?1) WHERE id=?2");
  update.bind(1, hintNo).bind(2, reviewId).step();
  tx.commit();
  return hint;
}
ReviewFeedback submitReview(sqlite3 *db, DictionaryPort &dictionary, std::int64_t reviewId,
                            const std::optional<std::string> &answer, UtcMillis now) {
  Transaction tx(db);
  auto pending = loadPending(db, reviewId);
  auto entry = dictionary.byId(db, pending.word_id);
  if (!entry)
    throw Error("review", "word data unavailable for review");
  const auto correct = answer && isCorrect(*answer, entry->word);
  const auto quality = qualityFor(correct, pending.hints_used);
  Stmt finish(db, "UPDATE reviews SET completed_at=?1, quality=?2 WHERE id=?3");
  finish.bind(1, formatUtc(now)).bind(2, qualityName(quality)).bind(3, reviewId).step();
  Stmt attempt(db, "INSERT INTO review_attempts(review_id,at,action,correct) "
                   "VALUES(?1,?2,?3,?4)");
  attempt.bind(1, reviewId)
      .bind(2, formatUtc(now))
      .bind(3, answer ? "answer" : "reveal")
      .bind(4, static_cast<std::int64_t>(correct ? 1 : 0))
      .step();
  auto memory = refreshWord(db, pending.word_id, now);
  tx.commit();
  ReviewFeedback feedback;
  feedback.correct = correct;
  feedback.quality = quality;
  feedback.answer = entry->display;
  feedback.hints_used = pending.hints_used;
  if (memory)
    feedback.memory = toDto(*memory);
  return feedback;
}
} // namespace lexica::parity::learning
