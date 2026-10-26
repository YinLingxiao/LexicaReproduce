#include "parity/app/application.h"
#include "test_support.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
namespace p = lexica::parity;
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct FixedClock : p::Clock {
  p::UtcMillis value;
  explicit FixedClock(p::UtcMillis v) : value(v) {}
  p::UtcMillis nowMillis() const override { return value; }
};
p::Json call(p::Application &app, const std::string &command, p::Json args = p::Json::object()) {
  static std::uint64_t sequence = 0;
  return app.dispatch({{"protocol_version", 1},
                       {"request_id", "library-" + std::to_string(++sequence)},
                       {"command", command},
                       {"args", std::move(args)}});
}
std::int64_t wordId(p::Application &app, const std::string &word) {
  std::int64_t id = 0;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement query(db, "SELECT id FROM words WHERE word=?");
    query.bind(1, word);
    check(query.step(), "missing seed word " + word);
    id = query.integer(0);
  });
  return id;
}
std::string memoryFact(p::Application &app, std::int64_t id) {
  std::string fact;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement query(db,
                       "SELECT printf('%.17g', strength_days), printf('%.17g', difficulty), "
                       "review_count, lapse_count, visit_count, weak_streak, high_priority, "
                       "first_visit_at, ifnull(last_visit_at,''), ifnull(last_review_at,''), "
                       "ifnull(last_reinforcement_at,''), ifnull(next_review_at,''), updated_at "
                       "FROM memory_states WHERE word_id=?");
    query.bind(1, id);
    check(query.step(), "missing memory row");
    for (int column = 0; column < 13; ++column)
      fact += query.text(column) + "|";
  });
  return fact;
}
void insertMemory(p::Application &app, std::int64_t id, const char *strength, int visits,
                  int priority, const char *lastVisit, const char *lastReview,
                  const char *reinforcement, const char *nextReview) {
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement insert(db,
                        "INSERT INTO memory_states(word_id, strength_days, difficulty, "
                        "review_count, lapse_count, visit_count, weak_streak, high_priority, "
                        "first_visit_at, last_visit_at, last_review_at, last_reinforcement_at, "
                        "next_review_at, updated_at) VALUES(?,?,0.3,1,0,?,0,?,?,?,?,?,?,?)");
    insert.bind(1, id).bind(2, std::string(strength)).bind(3, visits).bind(4, priority);
    insert.bind(5, "2026-01-01T00:00:00.000Z");
    if (lastVisit)
      insert.bind(6, lastVisit);
    else
      insert.null(6);
    if (lastReview)
      insert.bind(7, lastReview);
    else
      insert.null(7);
    if (reinforcement)
      insert.bind(8, reinforcement);
    else
      insert.null(8);
    if (nextReview)
      insert.bind(9, nextReview);
    else
      insert.null(9);
    insert.bind(10, "2026-01-01T00:00:00.000Z").step();
  });
}
std::string repeatUnit(const std::string &unit, int count) {
  std::string text;
  text.reserve(unit.size() * static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i)
    text += unit;
  return text;
}
void testNotes() {
  const auto now = p::parseUtc("2026-09-13T10:00:00.000Z");
  FixedClock clock(now);
  const auto dir = test_support::freshDirectory("library", "notes");
  const auto file = dir / "lexica.db";
  std::int64_t id = 0;
  const std::string original = "阅读笔记 100% _\n\"引号\" 😀 '; DROP TABLE word_notes;--";
  {
    p::Application app(file, &clock);
    const auto caps = app.capabilities();
    int ready = 0;
    for (const auto &command : caps["commands"])
      if (command["owner"] == "library" && command["available"] == true &&
          command["implementation"] == "implemented")
        ++ready;
    check(ready == 8, "library commands are not all implemented");
    id = wordId(app, "meticulous");
    const auto empty = call(app, "word_note", {{"wordId", id}});
    check(empty["ok"] == true && empty["result"]["bookmarked"] == false &&
              empty["result"]["note"] == "" && empty["result"]["ignored"] == false,
          "empty note defaults");
    const auto saved = call(app, "save_word_note",
                            {{"wordId", id}, {"bookmarked", true}, {"note", original}});
    check(saved["ok"] == true && saved["result"].is_null(), "save did not succeed");
    const auto loaded = call(app, "word_note", {{"wordId", id}});
    check(loaded["ok"] == true && loaded["result"]["bookmarked"] == true &&
              loaded["result"]["note"] == original,
          "saved note did not round-trip");
    app.database().withConnection([&](sqlite3 *db) {
      p::Statement stamp(db, "SELECT updated_at FROM word_notes WHERE word_id=?");
      stamp.bind(1, id);
      check(stamp.step() && stamp.text(0) == p::formatUtc(now), "updated_at is not the injected clock");
      p::Statement custom(db, "SELECT COUNT(*) FROM custom_wordbook_words WHERE word_id=?");
      custom.bind(1, id);
      check(custom.step() && custom.integer(0) == 0, "bookmark wrote a custom wordbook row");
      p::Statement memory(db, "SELECT COUNT(*) FROM memory_states WHERE word_id=?");
      memory.bind(1, id);
      check(memory.step() && memory.integer(0) == 0, "bookmark created memory");
    });
    const auto tooLong = call(app, "save_word_note",
                              {{"wordId", id},
                               {"bookmarked", false},
                               {"note", repeatUnit("字", 10001)}});
    check(tooLong["ok"] == false && tooLong["error"]["code"] == "BAD_REQUEST" &&
              std::string(tooLong["error"]["message"]).find("10,000") != std::string::npos,
          "overlong note was accepted");
    const auto kept = call(app, "word_note", {{"wordId", id}});
    check(kept["result"]["bookmarked"] == true && kept["result"]["note"] == original,
          "failed save replaced the original note");
    const auto missing = call(app, "save_word_note",
                              {{"wordId", id}, {"bookmarked", false}});
    check(missing["ok"] == false && missing["error"]["code"] == "BAD_REQUEST",
          "incomplete save reached the writer");
    check(call(app, "word_note", {{"wordId", id}})["result"]["note"] == original,
          "rejected request replaced the note");
    check(call(app, "save_word_note",
               {{"wordId", id}, {"bookmarked", true}, {"note", repeatUnit("字", 10000)}})["ok"] ==
              true,
          "10000 Chinese characters were rejected");
    check(call(app, "word_note", {{"wordId", id}})["result"]["note"] == repeatUnit("字", 10000),
          "10000 character note did not round-trip");
    check(call(app, "save_word_note",
               {{"wordId", -1}, {"bookmarked", true}, {"note", "x"}})["error"]["code"] ==
              "DB_ERROR",
          "unknown word reported success");
    check(call(app, "word_note", {{"wordId", id}})["result"]["bookmarked"] == true,
          "failed unknown-word save cleared the bookmark");
    const auto other = wordId(app, "elaborate");
    app.database().withConnection([&](sqlite3 *db) {
      p::Statement custom(db,
                          "INSERT INTO custom_wordbook_words(word_id, added_at) VALUES(?,?)");
      custom.bind(1, other).bind(2, p::formatUtc(now)).step();
    });
    check(call(app, "word_note", {{"wordId", other}})["result"]["bookmarked"] == false,
          "custom wordbook was reported as a bookmark");
    check(call(app, "library_words",
               {{"query", "elaborate"}, {"filter", "bookmarked"}, {"offset", 0}})["result"]["total"] ==
              0,
          "custom wordbook appeared in Saved");
    check(call(app, "library_words",
               {{"query", "elaborate"}, {"filter", "all"}, {"offset", 0}})["result"]["total"] == 0,
          "custom wordbook alone entered the vocabulary");
    check(call(app, "save_word_note",
               {{"wordId", other}, {"bookmarked", false}, {"note", ""}})["ok"] == true,
          "clearing an absent note failed");
    app.database().withConnection([&](sqlite3 *db) {
      p::Statement custom(db, "SELECT COUNT(*) FROM custom_wordbook_words WHERE word_id=?");
      custom.bind(1, other);
      check(custom.step() && custom.integer(0) == 1, "note save deleted the custom wordbook");
    });
  }
  {
    p::Application app(file, &clock);
    const auto loaded = call(app, "word_note", {{"wordId", id}});
    check(loaded["result"]["bookmarked"] == true &&
              loaded["result"]["note"] == repeatUnit("字", 10000),
          "note did not survive reopen");
  }
}
void testListing() {
  const auto base = p::parseUtc("2026-01-01T00:00:00.000Z");
  FixedClock clock(base);
  p::Application app(test_support::freshDirectory("library", "list") / "lexica.db", &clock);
  std::vector<std::pair<std::int64_t, std::string>> words;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement query(db, "SELECT id, word FROM words ORDER BY id LIMIT 25");
    while (query.step())
      words.push_back({query.integer(0), query.text(1)});
  });
  check(words.size() == 25, "seed did not yield 25 words");
  for (int i = 0; i < 25; ++i) {
    const auto touched = (i == 5) ? p::formatUtc(base + 8 * 1000) : p::formatUtc(base + i * 1000);
    insertMemory(app, words[static_cast<std::size_t>(i)].first, "10", i + 1, 0, touched.c_str(),
                 nullptr, touched.c_str(), "2026-02-01T00:00:00.000Z");
  }
  check(call(app, "save_word_note",
             {{"wordId", words[1].first},
              {"bookmarked", true},
              {"note", "阅读笔记 100% _"}})["ok"] == true,
        "bookmark save failed");
  check(call(app, "save_word_note",
             {{"wordId", words[2].first}, {"bookmarked", false}, {"note", "HelloNote"}})["ok"] ==
            true,
        "note save failed");
  const auto page = call(app, "library_words", {{"query", ""}, {"filter", "all"}, {"offset", 0}});
  check(page["ok"] == true && page["result"]["total"] == 25 && page["result"]["words"].size() == 24,
        "first page size or total is wrong");
  const auto next = call(app, "library_words", {{"query", "  "}, {"filter", "all"}, {"offset", 24}});
  check(next["result"]["total"] == 25 && next["result"]["words"].size() == 1, "second page is wrong");
  check(next["result"]["words"][0]["id"] == words[0].first, "oldest word was not the last page");
  const auto beyond =
      call(app, "library_words", {{"query", ""}, {"filter", "all"}, {"offset", 48}});
  check(beyond["result"]["total"] == 25 && beyond["result"]["words"].empty(),
        "offset past the end changed the total");
  std::vector<std::int64_t> ordered;
  for (const auto &row : page["result"]["words"])
    ordered.push_back(row["id"]);
  ordered.push_back(next["result"]["words"][0]["id"]);
  auto position = [&](std::int64_t id) {
    return static_cast<int>(std::find(ordered.begin(), ordered.end(), id) - ordered.begin());
  };
  check(position(words[24].first) == 0, "newest word was not first");
  check(position(words[8].first) + 1 == position(words[5].first),
        "equal timestamps were not ordered by descending id");
  check(position(words[5].first) < position(words[7].first),
        "older distinct timestamp sorted ahead of the tied pair");
  check(page["result"]["words"][0]["status"] == "familiar", "strength 10 was not familiar");
  bool sawChinese = false;
  for (const auto &row : page["result"]["words"])
    if (row["word"] == "meticulous")
      sawChinese = row["definition"] == "一丝不苟的；极其仔细的";
  for (const auto &row : next["result"]["words"])
    if (row["word"] == "meticulous")
      sawChinese = row["definition"] == "一丝不苟的；极其仔细的";
  check(sawChinese, "meticulous did not use its Chinese definition");
  const auto saved = call(app, "library_words",
                          {{"query", "  100% _  "}, {"filter", "bookmarked"}, {"offset", 0}});
  check(saved["result"]["total"] == 1 && saved["result"]["words"][0]["note"] == "阅读笔记 100% _" &&
            saved["result"]["words"][0]["bookmarked"] == true,
        "bookmark search missed the literal note");
  const auto noteHit = call(app, "library_words",
                            {{"query", "hellonote"}, {"filter", "all"}, {"offset", 0}});
  check(noteHit["result"]["total"] == 1 && noteHit["result"]["words"][0]["id"] == words[2].first &&
            noteHit["result"]["words"][0]["bookmarked"] == false,
        "case-insensitive note search failed");
  const auto trimmed = call(app, "library_words",
                            {{"query", "\xE3\x80\x80" + words[3].second + "\xE3\x80\x80"},
                             {"filter", "all"},
                             {"offset", 0}});
  check(trimmed["result"]["total"] == 1 && trimmed["result"]["words"][0]["id"] == words[3].first,
        "ideographic space was not trimmed");
  const auto upper = call(app, "library_words",
                          {{"query", words[3].second}, {"filter", "learning"}, {"offset", 0}});
  check(upper["result"]["total"] == 0, "learning filter kept a familiar word");
  const auto found = call(app, "library_words",
                          {{"query", words[3].second}, {"filter", "familiar"}, {"offset", 24}});
  check(found["result"]["total"] == 1 && found["result"]["words"].empty(),
        "filtered total did not survive a past-the-end offset");
  check(call(app, "library_words",
             {{"query", "NO-SUCH-XYZ"}, {"filter", "all"}, {"offset", 0}})["result"]["total"] == 0,
        "missing search was not an empty success");
  check(call(app, "set_word_ignored", {{"wordId", words[1].first}, {"ignored", true}})["ok"] == true,
        "pause failed");
  const auto paused = call(app, "library_words",
                           {{"query", words[1].second}, {"filter", "all"}, {"offset", 0}});
  check(paused["result"]["words"][0]["status"] == "ignored" &&
            paused["result"]["words"][0]["bookmarked"] == true &&
            paused["result"]["words"][0]["next_review_at"] == "2026-02-01T00:00:00.000Z",
        "paused word lost its bookmark or due time");
  check(call(app, "library_words",
             {{"query", ""}, {"filter", "ignored"}, {"offset", 0}})["result"]["total"] == 1,
        "paused filter count");
  check(call(app, "library_words",
             {{"query", ""}, {"filter", "familiar"}, {"offset", 0}})["result"]["total"] == 24,
        "paused word stayed in familiar");
  check(call(app, "library_words",
             {{"query", ""}, {"filter", "bookmarked"}, {"offset", 0}})["result"]["total"] == 1,
        "paused bookmark left Saved");
  check(call(app, "library_words",
             {{"query", "100% _"}, {"filter", "ignored"}, {"offset", 0}})["result"]["total"] == 1,
        "combined pause and note search failed");
  check(call(app, "library_words",
             {{"query", ""}, {"filter", "nope"}, {"offset", 0}})["result"]["total"] == 0,
        "unknown filter was not empty");
  const auto noDef = wordId(app, "hinder");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement word(db,
                      "INSERT INTO words(word, display, created_at) VALUES('nodef','No Def',?)");
    word.bind(1, "2026-01-01T00:00:00.000Z").step();
    const auto id = sqlite3_last_insert_rowid(db);
    p::Statement bare(db,
                      "INSERT INTO words(word, display, phonetic, created_at) "
                      "VALUES('enonly','En Only',NULL,?)");
    bare.bind(1, "2026-01-01T00:00:00.000Z").step();
    const auto englishId = sqlite3_last_insert_rowid(db);
    p::Statement first(db,
                       "INSERT INTO senses(word_id, english_definition, chinese_definition, order_idx) "
                       "VALUES(?, 'EN-ONLY', '', 0)");
    first.bind(1, englishId).step();
    p::Statement second(db,
                        "INSERT INTO senses(word_id, english_definition, chinese_definition, order_idx) "
                        "VALUES(?, 'EN-SECOND', '中文第二', 1)");
    second.bind(1, englishId).step();
    p::Statement mark(db,
                      "INSERT INTO word_notes(word_id, bookmarked, note, updated_at) VALUES(?,1,'',?)");
    mark.bind(1, id).bind(2, "2026-03-01T00:00:00.000Z").step();
    p::Statement markEnglish(db,
                             "INSERT INTO word_notes(word_id, bookmarked, note, updated_at) "
                             "VALUES(?,1,'',?)");
    markEnglish.bind(1, englishId).bind(2, "2026-03-02T00:00:00.000Z").step();
    (void)noDef;
  });
  const auto bare = call(app, "library_words", {{"query", "nodef"}, {"filter", "all"}, {"offset", 0}});
  check(bare["result"]["total"] == 1 && bare["result"]["words"][0]["definition"] == "No definition" &&
            bare["result"]["words"][0]["status"] == "new" &&
            bare["result"]["words"][0]["phonetic"].is_null() &&
            bare["result"]["words"][0]["visit_count"] == 0,
        "missing definition or new status");
  const auto english =
      call(app, "library_words", {{"query", "enonly"}, {"filter", "all"}, {"offset", 0}});
  check(english["result"]["words"][0]["definition"] == "EN-ONLY",
        "empty Chinese definition did not fall back to English");
  check(call(app, "library_words",
             {{"query", "EN-ONLY"}, {"filter", "all"}, {"offset", 0}})["result"]["total"] == 0,
        "definition text was searched");
  check(call(app, "library_words",
             {{"query", "ENONLY"}, {"filter", "bookmarked"}, {"offset", 0}})["result"]["words"][0]["word"] ==
            "enonly",
        "word search was case-sensitive");
  const auto early = p::formatUtc(base);
  const auto later = p::formatUtc(base + 50 * 1000);
  const auto hinderId = wordId(app, "hinder");
  insertMemory(app, hinderId, "10", 4, 0, early.c_str(), nullptr, early.c_str(), nullptr);
  check(call(app, "save_word_note",
             {{"wordId", hinderId}, {"bookmarked", false}, {"note", "later-note"}})["ok"] == true,
        "coalesce note save failed");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stamp(db, "UPDATE word_notes SET updated_at=? WHERE word_id=?");
    stamp.bind(1, later).bind(2, hinderId).step();
  });
  const auto hinder =
      call(app, "library_words", {{"query", "hinder"}, {"filter", "all"}, {"offset", 0}});
  const auto newest =
      call(app, "library_words", {{"query", ""}, {"filter", "all"}, {"offset", 0}});
  check(hinder["result"]["words"][0]["visit_count"] == 4, "visit count was not read from memory");
  bool hinderFirst = false;
  for (const auto &row : newest["result"]["words"])
    if (row["word"] == "hinder")
      hinderFirst = true;
  check(!hinderFirst, "note updated_at outranked an existing last_visit_at");
}
void testStats() {
  const auto now = p::parseUtc("2026-09-13T10:00:00.000Z");
  const auto active = p::formatUtc(now - 60 * 86400000LL);
  const auto stale = p::formatUtc(now - 60 * 86400000LL - 1);
  const auto current = p::formatUtc(now);
  const auto future = p::formatUtc(now + 86400000LL);
  const auto past = p::formatUtc(now - 86400000LL);
  const auto tenDays = p::formatUtc(now - 10 * 86400000LL);
  const auto ninety = p::formatUtc(now - 90 * 86400000LL);
  FixedClock clock(now);
  p::Application app(test_support::freshDirectory("library", "stats") / "lexica.db", &clock);
  const auto empty = call(app, "stats_summary");
  check(empty["ok"] == true && empty["result"]["encountered"] == 0 &&
            empty["result"]["learning"] == 0 && empty["result"]["familiar"] == 0 &&
            empty["result"]["stable"] == 0 && empty["result"]["ignored"] == 0 &&
            empty["result"]["due_now"] == 0 && empty["result"]["high_priority"] == 0,
        "empty stats were not zero");
  check(call(app, "fading_words")["result"].empty(), "empty fading list was not empty");
  const auto meticulous = wordId(app, "meticulous");
  const auto elaborate = wordId(app, "elaborate");
  const auto subtle = wordId(app, "subtle");
  const auto acquire = wordId(app, "acquire");
  const auto inevitable = wordId(app, "inevitable");
  const auto reluctant = wordId(app, "reluctant");
  const auto pragmatic = wordId(app, "pragmatic");
  const auto compelling = wordId(app, "compelling");
  const auto ambiguous = wordId(app, "ambiguous");
  insertMemory(app, meticulous, "3.999", 7, 0, current.c_str(), nullptr, tenDays.c_str(),
               current.c_str());
  insertMemory(app, elaborate, "4", 1, 0, current.c_str(), nullptr, current.c_str(), future.c_str());
  insertMemory(app, subtle, "20.999", 3, 1, current.c_str(), nullptr, past.c_str(),
               p::formatUtc(now - 1).c_str());
  insertMemory(app, acquire, "21", 2, 0, stale.c_str(), nullptr, stale.c_str(), past.c_str());
  insertMemory(app, inevitable, "30", 5, 1, current.c_str(), nullptr, current.c_str(), current.c_str());
  insertMemory(app, reluctant, "0.25", 4, 0, ninety.c_str(), active.c_str(), ninety.c_str(),
               current.c_str());
  insertMemory(app, pragmatic, "5", 6, 0, current.c_str(), nullptr, nullptr, current.c_str());
  check(call(app, "save_word_note",
             {{"wordId", compelling}, {"bookmarked", true}, {"note", "收藏不是词书"}})["ok"] == true,
        "bookmark fixture failed");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement custom(db, "INSERT INTO custom_wordbook_words(word_id, added_at) VALUES(?,?)");
    custom.bind(1, ambiguous).bind(2, current).step();
  });
  const auto before = memoryFact(app, subtle);
  const auto stats = call(app, "stats_summary")["result"];
  check(stats["encountered"] == 7 && stats["learning"] == 2 && stats["familiar"] == 3 &&
            stats["stable"] == 2 && stats["ignored"] == 0 && stats["due_now"] == 5 &&
            stats["high_priority"] == 2,
        "manual stats recount failed before pause");
  check(call(app, "set_word_ignored", {{"wordId", inevitable}, {"ignored", true}})["ok"] == true,
        "pause failed");
  const auto paused = call(app, "stats_summary")["result"];
  check(paused["encountered"] == 7 && paused["learning"] == 2 && paused["familiar"] == 3 &&
            paused["stable"] == 1 && paused["ignored"] == 1 && paused["due_now"] == 4 &&
            paused["high_priority"] == 1,
        "ignored word was still bucketed or still due");
  auto statusOf = [&](const std::string &word, const std::string &filter) {
    const auto page = call(app, "library_words", {{"query", word}, {"filter", filter}, {"offset", 0}});
    check(page["ok"] == true && page["result"]["total"] == 1, "status lookup failed for " + word);
    return page["result"]["words"][0]["status"].get<std::string>();
  };
  check(statusOf("meticulous", "learning") == "learning", "3.999 was not learning");
  check(statusOf("elaborate", "familiar") == "familiar", "4.0 was not familiar");
  check(statusOf("subtle", "familiar") == "familiar", "20.999 was not familiar");
  check(statusOf("acquire", "stable") == "stable", "21.0 was not stable");
  check(statusOf("inevitable", "ignored") == "ignored", "paused stable word stayed stable");
  check(statusOf("compelling", "bookmarked") == "new", "bookmark without memory was not new");
  check(call(app, "library_words",
             {{"query", "ambiguous"}, {"filter", "all"}, {"offset", 0}})["result"]["total"] == 0,
        "custom-only word entered the library");
  check(call(app, "library_words",
             {{"query", ""}, {"filter", "all"}, {"offset", 0}})["result"]["total"] == 8,
        "library population count");
  const auto dueWord = call(app, "library_words",
                            {{"query", "inevitable"}, {"filter", "ignored"}, {"offset", 0}});
  check(dueWord["result"]["words"][0]["next_review_at"] == current &&
            dueWord["result"]["words"][0]["visit_count"] == 5,
        "pause rewrote the due time or visit count");
  const auto fading = call(app, "fading_words")["result"];
  check(fading.size() == 4 && fading[0]["word"] == "subtle" && fading[0]["high_priority"] == true &&
            fading[1]["word"] == "pragmatic" && fading[1]["high_priority"] == false &&
            fading[2]["word"] == "reluctant" && fading[3]["word"] == "meticulous",
        "fading order was not priority then retrievability");
  check(call(app, "fading_words", {{"limit", 1}})["result"][0]["word"] == "subtle",
        "fading limit was ignored");
  check(call(app, "fading_words", {{"limit", 0}})["result"].empty(), "zero fading limit failed");
  check(call(app, "fading_words", {{"limit", 256}})["error"]["code"] == "BAD_REQUEST",
        "oversized limit was accepted");
  bool sawPaused = false;
  bool sawStale = false;
  for (const auto &row : fading) {
    sawPaused = sawPaused || row["word"] == "inevitable";
    sawStale = sawStale || row["word"] == "acquire";
  }
  check(!sawPaused, "paused word stayed in fading");
  check(!sawStale, "stale word stayed in fading");
  check(memoryFact(app, subtle) == before, "reading stats changed memory");
  const auto subtlePage = call(app, "library_words",
                               {{"query", "subtle"}, {"filter", "all"}, {"offset", 0}});
  const auto subtleDue = subtlePage["result"]["words"][0]["next_review_at"].get<std::string>();
  check(call(app, "set_word_ignored", {{"wordId", subtle}, {"ignored", true}})["ok"] == true,
        "second pause failed");
  const auto whilePaused = call(app, "stats_summary")["result"];
  check(whilePaused["familiar"] == 2 && whilePaused["ignored"] == 2 && whilePaused["due_now"] == 3 &&
            whilePaused["high_priority"] == 0 && whilePaused["stable"] == 1 &&
            whilePaused["learning"] == 2,
        "pausing a due familiar word did not leave the buckets");
  check(call(app, "library_words",
             {{"query", "subtle"}, {"filter", "familiar"}, {"offset", 0}})["result"]["total"] == 0,
        "paused word remained in familiar");
  check(memoryFact(app, subtle) == before, "pause rewrote memory");
  check(call(app, "set_word_ignored", {{"wordId", subtle}, {"ignored", false}})["ok"] == true,
        "resume failed");
  const auto restored = call(app, "stats_summary")["result"];
  check(restored["familiar"] == 3 && restored["ignored"] == 1 && restored["due_now"] == 4 &&
            restored["high_priority"] == 1,
        "resume did not restore the original buckets");
  const auto afterPage = call(app, "library_words",
                              {{"query", "subtle"}, {"filter", "all"}, {"offset", 0}});
  const auto after = afterPage["result"]["words"][0];
  check(after["status"] == "familiar" && after["next_review_at"] == subtleDue,
        "resume changed status or due time");
  check(memoryFact(app, subtle) == before, "resume rewrote memory");
  const auto faded = call(app, "fading_words", {{"limit", 1}});
  check(faded["result"][0]["word"] == "subtle" && faded["result"][0]["next_review_at"] == subtleDue,
        "resumed word did not return to fading with the same due time");
  const auto subtleNote = call(app, "word_note", {{"wordId", subtle}});
  const auto inevitableNote = call(app, "word_note", {{"wordId", inevitable}});
  check(subtleNote["result"]["ignored"] == false && inevitableNote["result"]["ignored"] == true,
        "ignored flag did not round-trip");
  const auto badPause = call(app, "set_word_ignored", {{"wordId", -1}, {"ignored", true}});
  check(badPause["error"]["code"] == "DB_ERROR", "pausing an unknown word succeeded");
  check(memoryFact(app, subtle) == before, "failed pause rewrote memory");
  check(call(app, "set_word_ignored", {{"wordId", -1}, {"ignored", false}})["ok"] == true,
        "resuming an unknown word failed");
}
void testActivity(p::UtcMillis now, const std::string &name) {
  FixedClock clock(now);
  p::Application app(test_support::freshDirectory("library", name) / "lexica.db", &clock);
  const auto emptyResponse = call(app, "activity_days", {{"offsetMinutes", 480}});
  const auto empty = emptyResponse["result"];
  check(empty.size() == 14 && empty[0]["date"] == "2026-08-31" && empty[13]["date"] == "2026-09-13" &&
            empty[1]["date"] == "2026-09-01",
        "empty UTC+8 window was not 14 local days");
  for (const auto &day : empty)
    check(day["lookups"] == 0 && day["reviews"] == 0, "empty day was not zero");
  const auto clampedResponse = call(app, "activity_days", {{"offsetMinutes", 10000}});
  const auto clamped = clampedResponse["result"];
  check(clamped[13]["date"] == "2026-09-14" && clamped[0]["date"] == "2026-09-01",
        "offset above 840 was not clamped");
  const auto negativeResponse = call(app, "activity_days", {{"offsetMinutes", -10000}});
  const auto negative = negativeResponse["result"];
  check(negative[13]["date"] == "2026-09-12" && negative[0]["date"] == "2026-08-30",
        "offset below -840 was not clamped");
  const auto id = wordId(app, "meticulous");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement oldEvent(db,
                          "INSERT INTO encounters(word_id, visited_at, source, comprehension_level) "
                          "VALUES(?, '2026-08-01T00:00:00.000Z', 'lookup', 'unknown')");
    oldEvent.bind(1, id).step();
  });
  const auto oldOnly = call(app, "activity_days", {{"offsetMinutes", 0}});
  for (const auto &day : oldOnly["result"])
    check(day["lookups"] == 0 && day["reviews"] == 0, "16-day-old lookup leaked into the window");
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DELETE FROM encounters");
    auto encounter = [&](const char *at, const char *source) {
      p::Statement row(db,
                       "INSERT INTO encounters(word_id, visited_at, source, comprehension_level) "
                       "VALUES(?,?,?, 'english')");
      row.bind(1, id).bind(2, at).bind(3, source).step();
    };
    encounter("2026-08-31T00:00:00.000Z", "lookup");
    encounter("2026-09-12T17:00:00.000Z", "lookup");
    encounter("2026-09-12T17:00:00.000Z", "review");
    p::Statement incomplete(db,
                            "INSERT INTO reviews(word_id, started_at, completed_at, quality) "
                            "VALUES(?, '2026-09-12T17:00:00.000Z', NULL, NULL)");
    incomplete.bind(1, id).step();
    const auto reviewId = sqlite3_last_insert_rowid(db);
    p::Statement attempt(db,
                         "INSERT INTO review_attempts(review_id, at, action, correct) "
                         "VALUES(?, '2026-09-12T17:00:00.000Z', 'answer', 1)");
    attempt.bind(1, reviewId).step();
    for (int i = 0; i < 2; ++i) {
      p::Statement done(db,
                        "INSERT INTO reviews(word_id, started_at, completed_at, quality) "
                        "VALUES(?, '2026-09-12T17:00:00.000Z', '2026-09-12T17:00:00.000Z', 'forgotten')");
      done.bind(1, id).step();
    }
  });
  const auto utc = call(app, "activity_days", {{"offsetMinutes", 0}})["result"];
  check(utc.size() == 14 && utc[0]["date"] == "2026-08-31" && utc[0]["lookups"] == 1 &&
            utc[0]["reviews"] == 0 && utc[12]["date"] == "2026-09-12" && utc[12]["lookups"] == 2 &&
            utc[12]["reviews"] == 2 && utc[13]["date"] == "2026-09-13" && utc[13]["lookups"] == 0 &&
            utc[13]["reviews"] == 0,
        "UTC activity counts or empty days were wrong");
  const auto shanghai = call(app, "activity_days", {{"offsetMinutes", 480}})["result"];
  check(shanghai[13]["date"] == "2026-09-13" && shanghai[13]["lookups"] == 2 &&
            shanghai[13]["reviews"] == 2 && shanghai[12]["date"] == "2026-09-12" &&
            shanghai[12]["lookups"] == 0 && shanghai[12]["reviews"] == 0 &&
            shanghai[0]["lookups"] == 1,
        "UTC+8 did not move 17:00Z onto the next local day");
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DELETE FROM review_attempts");
    p::execute(db, "DELETE FROM reviews");
    p::execute(db, "DELETE FROM encounters");
    auto add = [&](const char *at, bool review) {
      p::Statement row(db,
                       "INSERT INTO encounters(word_id, visited_at, comprehension_level) VALUES(?,?, 'unknown')");
      row.bind(1, id).bind(2, at).step();
      if (review) {
        p::Statement done(db,
                          "INSERT INTO reviews(word_id, started_at, completed_at, quality) "
                          "VALUES(?,?,?, 'good')");
        done.bind(1, id).bind(2, at).bind(3, at).step();
      }
    };
    add("2026-09-13T04:30:00.000Z", true);
    add("2026-09-13T05:30:00.000Z", false);
  });
  const auto eastern = call(app, "activity_days", {{"offsetMinutes", -300}})["result"];
  check(eastern[13]["date"] == "2026-09-13" && eastern[13]["lookups"] == 1 &&
            eastern[13]["reviews"] == 0 && eastern[12]["date"] == "2026-09-12" &&
            eastern[12]["lookups"] == 1 && eastern[12]["reviews"] == 1,
        "UTC-5 midnight split was wrong");
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DELETE FROM reviews");
    p::execute(db, "DELETE FROM encounters");
    auto add = [&](const char *at) {
      p::Statement row(db,
                       "INSERT INTO encounters(word_id, visited_at, comprehension_level) VALUES(?,?, 'context')");
      row.bind(1, id).bind(2, at).step();
    };
    add("2026-09-12T18:20:00.000Z");
    add("2026-09-12T18:40:00.000Z");
  });
  const auto india = call(app, "activity_days", {{"offsetMinutes", 330}})["result"];
  check(india[12]["date"] == "2026-09-12" && india[12]["lookups"] == 1 && india[12]["reviews"] == 0 &&
            india[13]["date"] == "2026-09-13" && india[13]["lookups"] == 1,
        "UTC+5:30 midnight split was wrong");
  std::int64_t lookups = 0;
  std::int64_t reviews = 0;
  for (const auto &day : india) {
    lookups += day["lookups"].get<std::int64_t>();
    reviews += day["reviews"].get<std::int64_t>();
    check(!day["date"].get<std::string>().empty(), "activity date was empty");
  }
  check(lookups == 2 && reviews == 0, "non-UTC+8 activity total was wrong");
}
void testRecent() {
  FixedClock clock(p::parseUtc("2026-09-13T10:00:00.000Z"));
  p::Application app(test_support::freshDirectory("library", "recent") / "lexica.db", &clock);
  check(call(app, "recent_words")["result"].empty(), "recent list was not empty");
  const auto first = wordId(app, "meticulous");
  const auto second = wordId(app, "subtle");
  app.database().withConnection([&](sqlite3 *db) {
    auto add = [&](std::int64_t id, const char *at, const char *level) {
      p::Statement row(db,
                       "INSERT INTO encounters(word_id, visited_at, comprehension_level) VALUES(?,?,?)");
      row.bind(1, id).bind(2, at).bind(3, level).step();
    };
    add(first, "2026-09-13T09:00:00.000Z", "unknown");
    add(first, "2026-09-13T08:00:00.000Z", "chinese");
    add(second, "2026-09-13T11:00:00.000Z", "english");
  });
  const auto recent = call(app, "recent_words", {{"limit", 6}})["result"];
  check(recent.size() == 2 && recent[0]["word"] == "subtle" &&
            recent[0]["comprehension_level"] == "english" &&
            recent[0]["visited_at"] == "2026-09-13T11:00:00.000Z" &&
            recent[0]["phonetic"] == "/ˈsʌtl/" && recent[1]["word"] == "meticulous" &&
            recent[1]["visited_at"] == "2026-09-13T08:00:00.000Z" &&
            recent[1]["comprehension_level"] == "chinese",
        "recent list did not keep the highest encounter id");
  check(call(app, "recent_words", {{"limit", 1}})["result"].size() == 1, "recent limit failed");
  check(call(app, "recent_words", {{"limit", 0}})["result"].empty(), "zero recent limit failed");
  check(call(app, "recent_words", {{"limit", -1}})["error"]["code"] == "BAD_REQUEST",
        "negative recent limit was accepted");
}
int main() {
  try {
    testNotes();
    testListing();
    testStats();
    testActivity(p::parseUtc("2026-09-13T10:00:00.000Z"), "activity");
    testRecent();
    std::cout << "Parity library: notes, pause, search, pages, stats, activity passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
