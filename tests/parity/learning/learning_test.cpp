#include "parity/app/application.h"
#include "parity/modules/learning/grader.h"
#include "parity/modules/learning/memory_service.h"
#include "parity/modules/learning/scheduler.h"
#include "parity/storage/database.h"
#include "test_support.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
namespace p = lexica::parity;
namespace l = lexica::parity::learning;
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct FakeClock : p::Clock {
  p::UtcMillis now = p::parseUtc("2026-01-01T00:00:00.000Z");
  p::UtcMillis nowMillis() const override { return now; }
};
std::uint64_t sequence = 0;
std::string nextId() { return "learn-" + std::to_string(++sequence); }
p::Json call(p::Application &app, const std::string &command, p::Json args,
             const std::string &id) {
  return app.dispatch({{"protocol_version", 1},
                       {"request_id", id},
                       {"command", command},
                       {"args", std::move(args)}});
}
p::Json ok(p::Application &app, const std::string &command,
           p::Json args = p::Json::object()) {
  auto response = call(app, command, std::move(args), nextId());
  if (response["ok"] != true)
    throw std::runtime_error(command + " " + response.dump());
  return response["result"];
}
void expectCode(const p::Json &response, const std::string &code) {
  check(response["ok"] == false && response["error"]["code"] == code,
        response.dump());
}
std::int64_t scalar(p::Application &app, const std::string &sql) {
  std::int64_t value = -1;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, sql);
    stmt.step();
    value = stmt.integer(0);
  });
  return value;
}
std::string cell(p::Application &app, const std::string &sql) {
  std::string value;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, sql);
    check(stmt.step(), sql);
    value = stmt.text(0);
  });
  return value;
}
double number(p::Application &app, const std::string &sql) {
  return std::stod(cell(app, sql));
}
std::int64_t idByName(p::Application &app, const std::string &word) {
  std::int64_t id = 0;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, "SELECT id FROM words WHERE word=?");
    stmt.bind(1, word).step();
    id = stmt.integer(0);
  });
  return id;
}
std::int64_t wordId(p::Application &app, const std::string &word) {
  return ok(app, "lookup_word", {{"word", word}})["outcome"]["entry"]["id"];
}
void testGrader() {
  check(l::blankWord("A word is a word.", "word") == "A ______ is a ______.",
        "repeat blank");
  check(!l::blankWord("The theme changed.", "he"), "boundary");
  check(l::blankWord("He agreed.", "he") == "______ agreed.", "start blank");
  check(l::maskHint("meticulous") == "m_________", "mask");
  check(l::maskHint("go") == "g_", "short mask");
  check(!l::isCorrect("on", "in") && !l::isCorrect("cut", "cat"),
        "short exact");
  check(l::isCorrect("Subtl", "subtle"), "distance one");
  check(l::isCorrect(" meticulous ", "meticulous"), "trim");
  check(l::isCorrect("ｍｅｔｉｃｕｌｏｕｓ", "meticulous"), "fullwidth");
  check(!l::isCorrect("Meticulosu", "meticulous"), "transpose");
  check(l::editDistance("kitten", "sitting") == 3, "edit");
  check(l::qualityFor(true, 0) == p::RecallQuality::Excellent, "q0");
  check(l::qualityFor(true, 1) == p::RecallQuality::Good, "q1");
  check(l::qualityFor(true, 2) == p::RecallQuality::Hard, "q2");
  check(l::qualityFor(true, 3) == p::RecallQuality::Poor, "q3");
  check(l::qualityFor(false, 0) == p::RecallQuality::Forgotten, "q wrong");
  p::WordEntry entry;
  entry.word = "zenith";
  entry.senses.push_back({1, "n.", "the highest point", "顶点", {}, {}});
  auto prompt = l::buildPrompt(entry);
  check(prompt.kind == p::PromptKind::Definition &&
            prompt.text == "the highest point",
        "definition prompt");
  entry.senses[0].english_definition.clear();
  prompt = l::buildPrompt(entry);
  check(prompt.kind == p::PromptKind::DefinitionZh && prompt.text == "顶点",
        "zh prompt");
  entry.senses[0].english_definition = "the highest point";
  entry.senses[0].examples.push_back(
      {7, "The sun reached its zenith at noon.", {}});
  prompt = l::buildPrompt(entry);
  check(prompt.kind == p::PromptKind::Cloze &&
            prompt.text == "The sun reached its ______ at noon." &&
            prompt.example_id == 7,
        "cloze prompt");
  auto local = l::buildDefinitionPrompt(entry);
  check(local.kind == p::PromptKind::DefinitionZh &&
            local.example_id == std::nullopt,
        "local zh fallback");
}
void testFold() {
  const auto t0 = p::parseUtc("2026-01-01T00:00:00.000Z");
  const std::pair<p::ComprehensionLevel, std::pair<double, std::int64_t>>
      levels[] = {{p::ComprehensionLevel::Context, {2.0, 172800000}},
                  {p::ComprehensionLevel::English, {1.0, 86400000}},
                  {p::ComprehensionLevel::Chinese, {0.5, 43200000}},
                  {p::ComprehensionLevel::Unknown, {0.25, 21600000}}};
  for (const auto &item : levels) {
    auto state = l::fold(1, {{false, t0, item.first, {}}});
    check(state && state->strength_days == item.second.first, "s0");
    check(state->next_review_at == t0 + item.second.second, "s0 due");
    check(state->visit_count == 1, "one visit");
  }
  check(!l::fold(1, {}), "empty");
  auto english = l::fold(1, {{false, t0, p::ComprehensionLevel::English, {}}});
  check(std::abs(l::retrievability(*english, t0) - 1.0) < 1e-12, "R now");
  check(std::abs(l::retrievability(*english, t0 + 86400000) - 0.9) < 1e-9,
        "R one S");
  check(std::abs(l::retrievability(*english, t0 + 172800000) - 0.81) < 1e-9,
        "R two S");
  const auto hour = 3600000LL;
  auto held =
      l::fold(1, {{false, t0, p::ComprehensionLevel::Chinese, {}},
                  {false, t0 + hour, p::ComprehensionLevel::English, {}},
                  {false, t0 + 20 * hour, p::ComprehensionLevel::Unknown, {}}});
  check(held->strength_days == 0.425 && held->difficulty == 0.35,
        "weak numbers");
  check(held->next_review_at == t0 + 43200000, "weak does not postpone");
  check(held->weak_streak == 2 && held->high_priority && held->visit_count == 3,
        "priority streak");
  auto reinforced =
      l::fold(1, {{false, t0, p::ComprehensionLevel::Chinese, {}},
                  {false, t0 + hour, p::ComprehensionLevel::English, {}},
                  {false, t0 + 20 * hour, p::ComprehensionLevel::Unknown, {}},
                  {true, t0 + 20 * hour, {}, p::RecallQuality::Good}});
  check(reinforced->strength_days == 0.733125 && reinforced->difficulty == 0.29,
        "good review");
  check(reinforced->next_review_at == t0 + 20 * hour + 63342000, "good due");
  check(!reinforced->high_priority && reinforced->weak_streak == 0 &&
            reinforced->review_count == 1,
        "priority cleared");
  const auto forgottenAt = t0 + 20 * hour + l::kDayMs;
  auto forgotten =
      l::fold(1, {{false, t0, p::ComprehensionLevel::Chinese, {}},
                  {false, t0 + hour, p::ComprehensionLevel::English, {}},
                  {false, t0 + 20 * hour, p::ComprehensionLevel::Unknown, {}},
                  {true, t0 + 20 * hour, {}, p::RecallQuality::Good},
                  {true, forgottenAt, {}, p::RecallQuality::Forgotten}});
  check(forgotten->strength_days == 0.25 && forgotten->lapse_count == 1,
        "lapse floor");
  check(forgotten->next_review_at == forgottenAt + 21600000, "lapse due");
  auto same = l::fold(1, {{false, t0, p::ComprehensionLevel::Chinese, {}},
                          {true, t0, {}, p::RecallQuality::Good}});
  auto reversed = l::fold(1, {{true, t0, {}, p::RecallQuality::Good},
                              {false, t0, p::ComprehensionLevel::Chinese, {}}});
  check(same->strength_days != reversed->strength_days,
        "same-instant order differs");
  check(same->visit_count == 1 && same->review_count == 1, "visit then review");
  check(reversed->visit_count == 1 && reversed->weak_streak == 1,
        "review then weak visit");
  auto context = l::fold(
      1, {{false, t0, p::ComprehensionLevel::Chinese, {}},
          {false, t0 + 11 * l::kDayMs, p::ComprehensionLevel::Context, {}}});
  check(context->strength_days == 0.625 && context->weak_streak == 0,
        "context gain");
  check(context->next_review_at == t0 + 11 * l::kDayMs + 54000000,
        "context due");
  check(l::statusOf(0.25) == p::MemoryStatus::Learning &&
            l::statusOf(4.0) == p::MemoryStatus::Familiar &&
            l::statusOf(21.0) == p::MemoryStatus::Stable,
        "thresholds");
}
p::Application openApp(const std::filesystem::path &file, FakeClock &clock) {
  return p::Application(file, &clock);
}
void testMemoryCommands(const std::filesystem::path &file) {
  FakeClock clock;
  auto app = openApp(file, clock);
  auto caps = call(app, "system_capabilities", p::Json::object(), nextId());
  check(caps["ok"] == true && caps["result"]["commands"].size() == 31,
        "catalog");
  for (const auto &command : caps["result"]["commands"])
    if (command["name"] == "lookup_word")
      check(command["implementation"] == "implemented", "lookup status");
  const auto t0 = clock.now;
  auto lookup = ok(app, "lookup_word", {{"word", "  Meticulous  "}});
  const auto id = lookup["outcome"]["entry"]["id"].get<std::int64_t>();
  const auto encounter = lookup["encounter_id"].get<std::int64_t>();
  check(lookup["outcome"]["type"] == "hit" &&
            lookup["comprehension_level"] == "unknown",
        "lookup event");
  check(lookup["memory"]["status"] == "learning" &&
            lookup["memory"]["visit_count"] == 1,
        "summary");
  check(std::abs(number(app, "SELECT strength_days FROM memory_states") -
                 0.25) < 1e-12,
        "stored s0");
  check(cell(app, "SELECT next_review_at FROM memory_states") ==
            p::formatUtc(t0 + 21600000),
        "stored due");
  ok(app, "set_comprehension",
     {{"encounterId", encounter}, {"level", "english"}});
  check(std::abs(number(app, "SELECT strength_days FROM memory_states") - 1.0) <
            1e-12,
        "refold english");
  clock.now = t0 + 1000;
  auto comp =
      call(app, "set_comprehension",
           {{"encounterId", encounter}, {"level", "english"}}, "comp-replay");
  const auto updated = cell(app, "SELECT updated_at FROM memory_states");
  clock.now = t0 + 2000;
  auto compReplay =
      call(app, "set_comprehension",
           {{"encounterId", encounter}, {"level", "english"}}, "comp-replay");
  check(comp["ok"] == true && compReplay["result"] == comp["result"],
        "comprehension replay result");
  check(cell(app, "SELECT updated_at FROM memory_states") == updated,
        "comprehension replay");
  expectCode(call(app, "set_comprehension",
                  {{"encounterId", encounter}, {"level", "context"}}, nextId()),
             "memory");
  clock.now = t0 + 600000;
  auto merged =
      call(app, "lookup_word", {{"word", "meticulous"}}, "lookup-replay");
  auto replay =
      call(app, "lookup_word", {{"word", "meticulous"}}, "lookup-replay");
  check(merged["ok"] == true && replay["result"] == merged["result"],
        "lookup replay");
  check(scalar(app, "SELECT COUNT(*) FROM encounters") == 1, "merged visit");
  check(cell(app, "SELECT visited_at FROM encounters") ==
            p::formatUtc(t0 + 600000),
        "replay keeps merged time");
  clock.now = t0 + 600000 + 1800000;
  auto again = ok(app, "lookup_word", {{"word", "meticulous"}});
  check(again["encounter_id"] != encounter, "merge boundary");
  expectCode(call(app, "set_comprehension",
                  {{"encounterId", encounter}, {"level", "chinese"}}, nextId()),
             "memory");
  expectCode(call(app, "study_group",
                  {{"wordbook", "cet4"},
                   {"dailyNewLimit", 1},
                   {"priority", "mixed"},
                   {"planStyle", "intensive"}},
                  nextId()),
             "BAD_REQUEST");
  (void)id;
}
void testProjection(const std::filesystem::path &file) {
  FakeClock clock;
  clock.now = p::parseUtc("2026-03-01T00:00:00.000Z");
  auto app = openApp(file, clock);
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement word(db, "INSERT INTO words(word,display,source,created_at) "
                          "VALUES('order-a','order-a','user',?)");
    word.bind(1, p::formatUtc(clock.now)).step();
    const auto first = sqlite3_last_insert_rowid(db);
    p::Statement word2(db, "INSERT INTO words(word,display,source,created_at) "
                           "VALUES('order-b','order-b','user',?)");
    word2.bind(1, p::formatUtc(clock.now)).step();
    const auto second = sqlite3_last_insert_rowid(db);
    const auto at = p::formatUtc(clock.now);
    p::Statement visit(db,
                       "INSERT INTO "
                       "encounters(id,word_id,visited_at,source,comprehension_"
                       "level) VALUES(101,?,?,'lookup','chinese')");
    visit.bind(1, first).bind(2, at).step();
    p::Statement review(
        db, "INSERT INTO reviews(id,word_id,started_at,completed_at,quality) "
            "VALUES(102,?,?,?,'good')");
    review.bind(1, first).bind(2, at).bind(3, at).step();
    p::Statement reviewFirst(
        db, "INSERT INTO reviews(id,word_id,started_at,completed_at,quality) "
            "VALUES(201,?,?,?,'good')");
    reviewFirst.bind(1, second).bind(2, at).bind(3, at).step();
    p::Statement visitLater(
        db, "INSERT INTO "
            "encounters(id,word_id,visited_at,source,comprehension_level) "
            "VALUES(202,?,?,'lookup','chinese')");
    visitLater.bind(1, second).bind(2, at).step();
    auto forward = l::fold(first, l::loadEvents(db, first));
    auto backward = l::fold(second, l::loadEvents(db, second));
    check(forward->review_count == 1 && forward->strength_days > 0.5,
          "db visit then review");
    check(backward->weak_streak == 1 &&
              backward->strength_days < forward->strength_days,
          "db review then visit");
    return 0;
  });
  check(ok(app, "rebuild_memory_projection") == 2, "rebuild count");
  auto again = call(app, "rebuild_memory_projection", p::Json::object(),
                    "rebuild-replay");
  check(again["ok"] == true && again["result"] == 2, "rebuild replay");
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "UPDATE memory_states SET strength_days=99");
    return 0;
  });
  ok(app, "rebuild_memory_projection");
  check(
      cell(app, "SELECT COUNT(*) FROM memory_states WHERE strength_days=99") ==
          "0",
      "rebuild restores");
  app.database().withConnection([&](sqlite3 *db) {
    auto events = l::loadEvents(db, 0);
    (void)events;
    p::Statement ids(db, "SELECT word_id, strength_days FROM memory_states");
    while (ids.step()) {
      auto folded = l::fold(ids.integer(0), l::loadEvents(db, ids.integer(0)));
      auto stored = l::loadProjection(db, ids.integer(0));
      check(folded && stored &&
                std::abs(folded->strength_days - stored->strength_days) < 1e-12,
            "projection matches fold");
    }
    p::execute(db, "DELETE FROM memory_states");
    return 0;
  });
  ok(app, "rebuild_memory_projection");
  check(scalar(app, "SELECT COUNT(*) FROM memory_states") == 2,
        "rebuild recreates");
}
void testQueue(const std::filesystem::path &file) {
  FakeClock clock;
  clock.now = p::parseUtc("2026-06-01T12:00:00.000Z");
  auto app = openApp(file, clock);
  auto idOf = [&](const std::string &word, p::UtcMillis at) {
    clock.now = at;
    return wordId(app, word);
  };
  const auto now = p::parseUtc("2026-06-01T12:00:00.000Z");
  const auto priority = idOf("undermine", now - 9 * l::kDayMs);
  idOf("undermine", now - 5 * l::kDayMs);
  idOf("undermine", now - 3 * l::kDayMs);
  idOf("undermine", now - l::kDayMs);
  const auto older = idOf("feasible", now - 10 * l::kDayMs);
  const auto recent = idOf("coherent", now - 2 * l::kDayMs);
  const auto future = idOf("acquire", now);
  const auto stale = idOf("versatile", now - 90 * l::kDayMs);
  const auto ignored = idOf("tentative", now - 3 * l::kDayMs);
  clock.now = now;
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, "INSERT INTO ignored_words(word_id) VALUES(?)");
    stmt.bind(1, ignored).step();
    return 0;
  });
  auto queue = ok(app, "review_queue");
  std::vector<std::int64_t> ids;
  for (const auto &item : queue)
    ids.push_back(item["word_id"]);
  auto pos = [&](std::int64_t id) {
    return std::find(ids.begin(), ids.end(), id);
  };
  check(pos(priority) == ids.begin(), "priority first");
  check(pos(older) != ids.end() && pos(recent) != ids.end() &&
            pos(older) < pos(recent),
        "lower R first");
  check(pos(future) == ids.end() && pos(stale) == ids.end() &&
            pos(ignored) == ids.end(),
        "filters");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, "DELETE FROM ignored_words WHERE word_id=?");
    stmt.bind(1, ignored).step();
    return 0;
  });
  ids.clear();
  auto restored = ok(app, "review_queue");
  for (const auto &item : restored)
    ids.push_back(item["word_id"]);
  check(std::find(ids.begin(), ids.end(), ignored) != ids.end(), "unignore");
  for (const char *word : {"meticulous", "subtle", "elaborate", "inevitable",
                           "reluctant", "compelling", "profound", "ambiguous",
                           "resilient", "pragmatic", "rigorous"})
    idOf(word, now - 3 * l::kDayMs);
  clock.now = now;
  const auto before = scalar(app, "SELECT COUNT(*) FROM encounters");
  auto grouped = ok(app, "review_group");
  auto queued = ok(app, "review_queue");
  check(grouped.size() == 10, "group cap " + std::to_string(grouped.size()));
  check(queued.size() == 10, "queue cap " + std::to_string(queued.size()));
  check(scalar(app, "SELECT COUNT(*) FROM encounters") == before,
        "group read only");
  check(scalar(app, "SELECT COUNT(*) FROM reviews") == 0, "group no reviews");
}
void testStudy(const std::filesystem::path &file) {
  FakeClock clock;
  clock.now = p::parseUtc("2026-08-01T00:00:00.000Z");
  auto app = openApp(file, clock);
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "UPDATE words SET tags='', oxford=0");
    return 0;
  });
  clock.now = p::parseUtc("2026-07-29T00:00:00.000Z");
  const auto due = wordId(app, "meticulous");
  clock.now = p::parseUtc("2026-08-01T00:00:00.000Z");
  auto group = ok(app, "study_group",
                  {{"wordbook", "cet4"},
                   {"dailyNewLimit", 2},
                   {"priority", "review_first"}});
  check(group["due_count"].get<int>() >= 1, "fallback due");
  check(group["cards"].size() == 3, "fallback quota");
  bool sawDue = false;
  for (const auto &card : group["cards"])
    if (card["entry"]["id"] == due)
      sawDue = !card["is_new"].get<bool>();
  check(sawDue, "due card");
  const auto subtle = idByName(app, "subtle");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, "UPDATE words SET tags='CET4 cet6' WHERE id=?");
    stmt.bind(1, subtle).step();
    return 0;
  });
  group = ok(
      app, "study_group",
      {{"wordbook", "cet4"}, {"dailyNewLimit", 20}, {"priority", "new_first"}});
  bool sawSubtle = false;
  bool sawUntaggedDue = false;
  for (const auto &card : group["cards"]) {
    if (card["entry"]["id"] == subtle)
      sawSubtle = card["is_new"];
    if (card["entry"]["id"] == due)
      sawUntaggedDue = !card["is_new"].get<bool>();
  }
  check(sawSubtle && sawUntaggedDue && group["new_count"] == 1,
        "tag plus unfiltered due");
  const auto oxford = idByName(app, "acquire");
  app.database().withConnection([&](sqlite3 *db) {
    p::Statement stmt(db, "UPDATE words SET oxford=1 WHERE id=?");
    stmt.bind(1, oxford).step();
    p::Statement note(
        db, "INSERT INTO word_notes(word_id,bookmarked,note,updated_at) "
            "VALUES(?,1,'n',?)");
    note.bind(1, oxford).bind(2, p::formatUtc(clock.now)).step();
    return 0;
  });
  ok(app, "custom_wordbook_toggle", {{"wordId", subtle}, {"added", true}});
  ok(app, "custom_wordbook_toggle", {{"wordId", subtle}, {"added", true}});
  group = ok(app, "study_group",
             {{"wordbook", "custom"},
              {"dailyNewLimit", 20},
              {"priority", "new_first"}});
  bool customFlag = false, bookmarkFlag = false;
  for (const auto &card : group["cards"]) {
    if (card["entry"]["id"] == subtle)
      customFlag = card["in_custom_wordbook"];
    if (card["entry"]["id"] == oxford)
      bookmarkFlag = card["bookmarked"];
  }
  check(customFlag && bookmarkFlag, "custom flags");
  auto books = ok(app, "wordbooks_list");
  check(books.size() == 9, "nine books");
  for (const auto &book : books)
    if (book["id"] == "custom")
      check(book["total_words"] == 2, "custom total");
  ok(app, "custom_wordbook_toggle", {{"wordId", subtle}, {"added", false}});
  clock.now = p::parseUtc("2026-08-01T00:00:00.000Z");
  auto encountered = ok(app, "study_group",
                        {{"wordbook", "encountered"},
                         {"dailyNewLimit", 20},
                         {"priority", "mixed"}});
  for (const auto &card : encountered["cards"])
    check(card["is_new"] == false, "encountered has no new cards");
  expectCode(call(app, "study_group",
                  {{"wordbook", "unknown"},
                   {"dailyNewLimit", 1},
                   {"priority", "mixed"}},
                  nextId()),
             "review");
  const auto before = scalar(app, "SELECT COUNT(*) FROM encounters");
  ok(app, "study_group",
     {{"wordbook", "cet4"}, {"dailyNewLimit", 0}, {"priority", "mixed"}});
  check(scalar(app, "SELECT COUNT(*) FROM encounters") == before,
        "browse writes nothing");
  check(scalar(app, "SELECT COUNT(*) FROM reviews") == 0, "browse no grades");
}
void testStudyLimit(const std::filesystem::path &file) {
  FakeClock clock;
  clock.now = p::parseUtc("2026-10-20T00:00:00.000Z");
  auto app = openApp(file, clock);
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "UPDATE words SET tags=''");
    for (int i = 0; i < 50; ++i) {
      const auto name = "studyword" + std::to_string(i);
      p::Statement word(
          db, "INSERT INTO words(word,display,source,created_at,tags) "
              "VALUES(?,?, 'user', ?, 'cet4')");
      word.bind(1, name).bind(2, name).bind(3, p::formatUtc(clock.now)).step();
      const auto id = sqlite3_last_insert_rowid(db);
      p::Statement sense(db, "INSERT INTO senses(word_id,english_definition) "
                             "VALUES(?, 'A vocabulary test word')");
      sense.bind(1, id).step();
      std::optional<p::UtcMillis> at;
      if (i < 25)
        at = clock.now - 3 * l::kDayMs;
      else if (i == 25)
        at = clock.now;
      else if (i == 26)
        at = clock.now - 1;
      if (at) {
        p::Statement visit(db,
                           "INSERT INTO "
                           "encounters(word_id,visited_at,source,comprehension_"
                           "level) VALUES(?,?,'lookup','unknown')");
        visit.bind(1, id).bind(2, p::formatUtc(*at)).step();
      }
    }
    return 0;
  });
  ok(app, "rebuild_memory_projection");
  for (const char *priority : {"mixed", "new_first", "review_first"}) {
    auto group = ok(
        app, "study_group",
        {{"wordbook", "cet4"}, {"dailyNewLimit", 100}, {"priority", priority}});
    check(group["cards"].size() == 20, priority);
    check(group["today_learned_new"] == 1, "utc quota");
    check(group["due_count"] == 25, "due count is uncapped");
    check(group["new_count"] == 20, "new count before trim");
    int fresh = 0;
    for (const auto &card : group["cards"])
      if (card["is_new"])
        ++fresh;
    const int expected = std::string(priority) == "mixed"       ? 10
                         : std::string(priority) == "new_first" ? 20
                                                                : 0;
    check(fresh == expected, std::string(priority) + " mix");
  }
}
void testReview(const std::filesystem::path &file) {
  FakeClock clock;
  clock.now = p::parseUtc("2026-09-01T00:00:00.000Z");
  auto app = openApp(file, clock);
  clock.now = p::parseUtc("2026-08-29T00:00:00.000Z");
  const auto id = wordId(app, "meticulous");
  clock.now = p::parseUtc("2026-09-01T00:00:00.000Z");
  auto queue = ok(app, "review_queue");
  bool found = false;
  for (const auto &item : queue)
    if (item["word_id"] == id) {
      found = true;
      check(item["kind"] == "definition_zh" && item["source"] == "dictionary",
            "zh kind");
      check(item["prompt"].get<std::string>().find("meticulous") ==
                std::string::npos,
            "hidden");
      check(item["example_id"].is_null(), "no example");
    }
  check(found, "due prompt");
  auto grade = call(app, "study_grade", {{"wordId", id}, {"grade", "know"}},
                    "grade-same");
  auto gradeReplay = call(app, "study_grade",
                          {{"wordId", id}, {"grade", "know"}}, "grade-same");
  check(grade["ok"] == true && gradeReplay["result"] == grade["result"],
        "grade replay");
  check(scalar(app, "SELECT COUNT(*) FROM reviews") == 1, "replay one review");
  expectCode(call(app, "study_grade", {{"wordId", id}, {"grade", "forgot"}},
                  "grade-same"),
             "BAD_REQUEST");
  check(scalar(app, "SELECT COUNT(*) FROM reviews") == 1,
        "mismatched replay writes nothing");
  auto second = ok(app, "study_grade", {{"wordId", id}, {"grade", "know"}});
  check(second["quality"] == "good" && second["correct"] == true, "new grade");
  check(scalar(app, "SELECT COUNT(*) FROM reviews") == 2,
        "new request records");
  check(scalar(app, "SELECT COUNT(*) FROM encounters") == 1,
        "grade reuses visit");
  ok(app, "study_grade", {{"wordId", id}, {"grade", "hazy"}});
  ok(app, "study_grade", {{"wordId", id}, {"grade", "forgot"}});
  check(cell(app, "SELECT quality FROM reviews ORDER BY id DESC LIMIT 1") ==
            "forgotten",
        "forgot");
  check(scalar(app, "SELECT lapse_count FROM memory_states WHERE word_id=" +
                        std::to_string(id)) == 1,
        "lapse stored");
  const auto attempts = scalar(app, "SELECT COUNT(*) FROM review_attempts");
  check(ok(app, "practice_check", {{"wordId", id}, {"answer", "meticulous"}}) ==
            true,
        "practice hit");
  check(ok(app, "practice_check", {{"wordId", id}, {"answer", "subtle"}}) ==
            false,
        "practice miss");
  check(ok(app, "practice_check", {{"wordId", id}, {"answer", "meticulus"}}) ==
            true,
        "practice typo");
  check(ok(app, "practice_check", {{"wordId", id}, {"answer", nullptr}}) ==
            false,
        "practice reveal");
  check(ok(app, "practice_hint",
           {{"wordId", id}, {"senseId", nullptr}, {"hintNo", 1}})["kind"] ==
            "mask",
        "practice mask");
  check(scalar(app, "SELECT COUNT(*) FROM review_attempts") == attempts,
        "practice not counted");
  expectCode(call(app, "practice_hint",
                  {{"wordId", id}, {"senseId", nullptr}, {"hintNo", 0}},
                  nextId()),
             "BAD_REQUEST");
  auto started =
      call(app, "start_review_item",
           {{"wordId", id}, {"senseId", nullptr}, {"exampleId", nullptr}},
           "start-replay");
  auto startedAgain =
      call(app, "start_review_item",
           {{"wordId", id}, {"senseId", nullptr}, {"exampleId", nullptr}},
           "start-replay");
  check(started["result"] == startedAgain["result"], "start replay");
  const auto reviewId = started["result"].get<std::int64_t>();
  expectCode(call(app, "request_hint", {{"reviewId", reviewId}, {"hintNo", 2}},
                  nextId()),
             "review");
  auto hint = call(app, "request_hint", {{"reviewId", reviewId}, {"hintNo", 1}},
                   "hint-replay");
  auto hintAgain = call(app, "request_hint",
                        {{"reviewId", reviewId}, {"hintNo", 1}}, "hint-replay");
  check(hint["result"] == hintAgain["result"] &&
            hint["result"]["kind"] == "mask",
        "hint replay");
  check(scalar(app, "SELECT hints_used FROM reviews WHERE id=" +
                        std::to_string(reviewId)) == 1,
        "one hint row");
  ok(app, "request_hint", {{"reviewId", reviewId}, {"hintNo", 2}});
  auto submitted =
      call(app, "submit_review",
           {{"reviewId", reviewId}, {"answer", "meticulous"}}, "submit-replay");
  auto submittedAgain =
      call(app, "submit_review",
           {{"reviewId", reviewId}, {"answer", "meticulous"}}, "submit-replay");
  check(submitted["ok"] == true &&
            submittedAgain["result"] == submitted["result"],
        "submit replay");
  check(submitted["result"]["correct"] == true &&
            submitted["result"]["quality"] == "hard",
        "two hints hard");
  check(submitted["result"]["memory"]["review_count"] == 5,
        "formal review counted");
  expectCode(call(app, "submit_review",
                  {{"reviewId", reviewId}, {"answer", "x"}}, nextId()),
             "review");
  check(scalar(app, "SELECT COUNT(*) FROM reviews WHERE id=" +
                        std::to_string(reviewId) +
                        " AND completed_at IS NOT NULL") == 1,
        "formal once");
  const auto reveal =
      ok(app, "start_review_item",
         {{"wordId", id}, {"senseId", nullptr}, {"exampleId", nullptr}});
  auto revealed =
      ok(app, "submit_review", {{"reviewId", reveal}, {"answer", nullptr}});
  check(revealed["correct"] == false && revealed["quality"] == "forgotten",
        "reveal");
  const auto entry =
      ok(app, "lookup_word", {{"word", "elaborate"}})["outcome"]["entry"];
  const auto sense = entry["senses"][1];
  p::Json example = nullptr;
  if (!sense["examples"].empty())
    example = sense["examples"][0]["id"];
  const auto detailed = ok(app, "start_review_item",
                           {{"wordId", entry["id"]},
                            {"senseId", sense["id"]},
                            {"exampleId", example}});
  ok(app, "request_hint", {{"reviewId", detailed}, {"hintNo", 1}});
  auto english =
      ok(app, "request_hint", {{"reviewId", detailed}, {"hintNo", 2}});
  check(english["text"] == sense["english_definition"], "sense hint");
  expectCode(call(app, "start_review_item",
                  {{"wordId", id}, {"senseId", 999999}, {"exampleId", nullptr}},
                  nextId()),
             "review");
  expectCode(
      call(app, "study_grade", {{"wordId", -1}, {"grade", "know"}}, nextId()),
      "review");
  expectCode(call(app, "submit_review",
                  {{"reviewId", 999999}, {"answer", nullptr}}, nextId()),
             "review");
}
void testRollback(const std::filesystem::path &file) {
  FakeClock clock;
  auto app = openApp(file, clock);
  const auto id = wordId(app, "rigorous");
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DELETE FROM encounters");
    p::execute(db, "DELETE FROM memory_states");
    p::execute(db,
               "CREATE TRIGGER reject_study_review BEFORE INSERT ON reviews "
               "BEGIN SELECT RAISE(ABORT, 'test failure'); END;");
    return 0;
  });
  expectCode(call(app, "study_grade", {{"wordId", id}, {"grade", "know"}},
                  "rollback-grade"),
             "DB_ERROR");
  check(scalar(app, "SELECT COUNT(*) FROM encounters") == 0,
        "rolled encounter");
  check(scalar(app, "SELECT COUNT(*) FROM memory_states") == 0,
        "rolled projection");
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DROP TRIGGER reject_study_review");
    return 0;
  });
  check(call(app, "study_grade", {{"wordId", id}, {"grade", "know"}},
             "rollback-grade")["ok"] == true,
        "retry after rollback");
  check(scalar(app, "SELECT COUNT(*) FROM encounters") == 1,
        "recovered encounter");
  check(scalar(app, "SELECT COUNT(*) FROM reviews") == 1, "recovered review");
}
void testIgnoredAndMissing(const std::filesystem::path &file) {
  FakeClock clock;
  clock.now = p::parseUtc("2026-04-10T00:00:00.000Z");
  auto app = openApp(file, clock);
  app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "UPDATE words SET tags=''");
    p::Statement word(db,
                      "INSERT INTO words(word,display,source,created_at,tags) "
                      "VALUES('empty-def','empty-def','user',?,'cet4')");
    word.bind(1, p::formatUtc(clock.now)).step();
    p::Statement sense(
        db, "INSERT INTO senses(word_id,english_definition,chinese_definition) "
            "VALUES(?, '', NULL)");
    sense.bind(1, sqlite3_last_insert_rowid(db)).step();
    p::Statement zh(db,
                    "INSERT INTO words(word,display,source,created_at,tags) "
                    "VALUES('zh-only','zh-only','user',?,'cet4')");
    zh.bind(1, p::formatUtc(clock.now)).step();
    const auto zhId = sqlite3_last_insert_rowid(db);
    p::Statement zhSense(
        db, "INSERT INTO senses(word_id,english_definition,chinese_definition) "
            "VALUES(?, '', '只有中文')");
    zhSense.bind(1, zhId).step();
    return 0;
  });
  auto group = ok(
      app, "study_group",
      {{"wordbook", "cet4"}, {"dailyNewLimit", 20}, {"priority", "new_first"}});
  bool sawEmpty = false, sawZh = false;
  for (const auto &card : group["cards"]) {
    if (card["entry"]["word"] == "empty-def")
      sawEmpty = true;
    if (card["entry"]["word"] == "zh-only") {
      sawZh = true;
      check(card["item"]["kind"] == "definition_zh", "zh card");
    }
  }
  check(!sawEmpty && sawZh, "skip missing material");
}
class BlankDictionary : public p::DictionaryPort {
public:
  std::optional<p::WordEntry> find(sqlite3 *, const std::string &) override {
    return std::nullopt;
  }
  std::optional<p::WordEntry> byId(sqlite3 *, std::int64_t) override {
    return std::nullopt;
  }
  std::vector<p::Suggestion> suggest(sqlite3 *, const std::string &,
                                     int) override {
    return {};
  }
};
class FixedDictionary : public p::DictionaryPort {
public:
  p::WordEntry entry;
  std::optional<p::WordEntry> find(sqlite3 *,
                                   const std::string &word) override {
    return word == "from-port" ? std::optional<p::WordEntry>{entry}
                               : std::nullopt;
  }
  std::optional<p::WordEntry> byId(sqlite3 *, std::int64_t id) override {
    return id == entry.id ? std::optional<p::WordEntry>{entry} : std::nullopt;
  }
  std::vector<p::Suggestion> suggest(sqlite3 *, const std::string &,
                                     int) override {
    return {};
  }
};
void testPort(const std::filesystem::path &file) {
  FakeClock clock;
  p::Database db(file);
  db.seed();
  std::int64_t id = 0;
  db.withConnection([&](sqlite3 *raw) {
    p::Statement stmt(raw, "SELECT id FROM words WHERE word='meticulous'");
    stmt.step();
    id = stmt.integer(0);
    return 0;
  });
  BlankDictionary blank;
  p::Router router;
  p::registerLearning(router);
  p::Context blankContext{db, clock, blank, {}, {}};
  auto miss = router.dispatch({{"protocol_version", 1},
                               {"request_id", "port-miss"},
                               {"command", "lookup_word"},
                               {"args", {{"word", "meticulous"}}}},
                              blankContext);
  check(miss["ok"] == true && miss["result"]["outcome"]["type"] == "miss",
        "port miss");
  db.withConnection([&](sqlite3 *raw) {
    p::Statement stmt(raw, "SELECT COUNT(*) FROM encounters");
    stmt.step();
    check(stmt.integer(0) == 0, "miss writes nothing");
    return 0;
  });
  FixedDictionary fixed;
  fixed.entry.id = id;
  fixed.entry.word = "from-port";
  fixed.entry.display = "From Port";
  fixed.entry.senses.push_back({1, {}, "careful", "仔细", {}, {}});
  p::Context fixedContext{db, clock, fixed, {}, {}};
  auto hit = router.dispatch({{"protocol_version", 1},
                              {"request_id", "port-hit"},
                              {"command", "lookup_word"},
                              {"args", {{"word", "from-port"}}}},
                             fixedContext);
  check(hit["ok"] == true &&
            hit["result"]["outcome"]["entry"]["display"] == "From Port",
        "port hit");
  check(hit["result"]["encounter_id"].is_number(), "port encounter");
  auto stillMiss = router.dispatch({{"protocol_version", 1},
                                    {"request_id", "port-still"},
                                    {"command", "lookup_word"},
                                    {"args", {{"word", "meticulous"}}}},
                                   fixedContext);
  check(stillMiss["result"]["outcome"]["type"] == "miss",
        "db word ignored without port");
}
void testReceiptIsolation(const std::filesystem::path &root) {
  FakeClock clock;
  const auto one = root / "receipt-one.db", two = root / "receipt-two.db";
  {
    p::Application first(one, &clock), second(two, &clock);
    const auto args = p::Json{{"word", "meticulous"}};
    check(call(first, "lookup_word", args, "same-client-id")["ok"] == true,
          "first receipt");
    check(call(second, "lookup_word", args, "same-client-id")["ok"] == true,
          "second receipt");
    check(scalar(first, "SELECT COUNT(*) FROM encounters") == 1,
          "first encounter missing");
    check(scalar(second, "SELECT COUNT(*) FROM encounters") == 1,
          "receipt leaked across databases");
    const auto grade =
        p::Json{{"wordId", idByName(first, "meticulous")}, {"grade", "know"}};
    check(call(first, "study_grade", grade, "same-grade-id")["ok"] == true,
          "first grade");
    check(call(second, "study_grade", grade, "same-grade-id")["ok"] == true,
          "second grade");
    check(
        scalar(second,
               "SELECT COUNT(*) FROM reviews WHERE completed_at IS NOT NULL") ==
            1,
        "grade receipt leaked across databases");
    check(call(second, "study_grade", grade, "same-grade-id")["ok"] == true,
          "grade retry");
    check(
        scalar(second,
               "SELECT COUNT(*) FROM reviews WHERE completed_at IS NOT NULL") ==
            1,
        "grade retry duplicated");
  }
  clock.now += 86400000;
  p::Application reopened(one, &clock);
  check(call(reopened, "lookup_word", {{"word", "meticulous"}},
             "same-client-id")["ok"] == true,
        "reopened lookup");
  check(scalar(reopened, "SELECT COUNT(*) FROM encounters") == 2,
        "receipt outlived Application");
}
int main() {
  try {
    testGrader();
    testFold();
    const auto dir = test_support::freshDirectory("parity", "learning");
    testMemoryCommands(dir / "memory.db");
    testProjection(dir / "projection.db");
    testQueue(dir / "queue.db");
    testStudy(dir / "study.db");
    testStudyLimit(dir / "limit.db");
    testReview(dir / "review.db");
    testRollback(dir / "rollback.db");
    testIgnoredAndMissing(dir / "missing.db");
    testPort(dir / "port.db");
    testReceiptIsolation(dir);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
