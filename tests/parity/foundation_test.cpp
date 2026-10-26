#include "parity/app/application.h"
#include "parity/contracts/catalog.h"
#include "test_support.h"
#include <fstream>
#include <iostream>
namespace p = lexica::parity;
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct FixedClock : p::Clock {
  p::UtcMillis nowMillis() const override { return 1792540800123LL; }
};
p::Json call(p::Application &app, const std::string &command,
             p::Json args = p::Json::object()) {
  static std::uint64_t sequence = 0;
  return app.dispatch(
      {{"protocol_version", 1},
       {"request_id", "foundation-" + std::to_string(++sequence)},
       {"command", command},
       {"args", std::move(args)}});
}
int main() {
  try {
    const auto dir = test_support::freshDirectory("parity", "foundation");
    const auto file = dir / L"数据库" / L"lexica.db";
    FixedClock clock;
    std::int64_t wordId = 0;
    {
      p::Application app(file, &clock);
      bool readOnlySeed = false;
      const auto capabilities = app.capabilities();
      for (const auto &command : capabilities["commands"])
        if (command["name"] == "lookup_word")
          readOnlySeed = command["implementation"] == "read_only_seed";
      const auto info = call(app, "app_info");
      check(info["ok"] == true, "app_info failed");
      check(info["result"]["schema_version"] == 5 &&
                info["result"]["word_count"] == 55 &&
                info["result"]["fts_ok"] == true,
            "schema/seed/FTS incorrect");
      check(info["result"]["dictionary_ready"] == false,
            "Seed DB must not masquerade as full ECDICT");
      const auto lookup =
          call(app, "lookup_word", {{"word", "  Meticulous  "}});
      check(lookup["ok"] == true &&
                lookup["result"]["outcome"]["type"] == "hit",
            "lookup did not use SQL");
      const auto entry =
          lookup["result"]["outcome"]["entry"].get<p::WordEntry>();
      wordId = entry.id;
      check(entry.word == "meticulous" && entry.senses.size() == 1 &&
                entry.senses[0].examples.size() == 3,
            "nested DTO/seed data lost");
      check(entry.senses[0].examples[0].translation.has_value(),
            "nullable translation lost");
      check(call(app, "lookup_word",
                 {{"word",
                   "lexica-no-such-word"}})["result"]["outcome"]["type"] ==
                "miss",
            "miss discriminant incorrect");
      check(call(app, "search_suggest",
                 {{"query", "met"}})["result"][0]["word"] == "meticulous",
            "default limit/suggestion failed");
      check(call(app, "search_suggest",
                 {{"query", "met"}, {"limit", 0}})["result"]
                .empty(),
            "zero limit failed");
      for (const auto &bad :
           p::Json::array({p::Json{{"query", true}},
                           p::Json{{"query", "met"}, {"limit", -1}},
                           p::Json{{"query", "met"}, {"extra", 1}},
                           p::Json{{"query", "met"}, {"limit", 1.5}}}))
        check(call(app, "search_suggest", bad)["error"]["code"] ==
                  "BAD_REQUEST",
              "invalid args reached handler");
      check(call(app, "study_grade",
                 {{"wordId", wordId},
                  {"grade", "not-a-grade"}})["error"]["code"] == "BAD_REQUEST",
            "enum validation failed");
      check(call(app, "unknown_command")["error"]["code"] == "UNKNOWN_COMMAND",
            "unknown command status");
      check(app.dispatch({{"protocol_version", 2},
                          {"request_id", "bad"},
                          {"command", "app_info"}})["error"]["code"] ==
                "PROTOCOL_MISMATCH",
            "protocol validation failed");
      check(app.dispatch(p::Json::array())["error"]["code"] == "BAD_REQUEST",
            "malformed envelope failed");
      const auto caps = app.capabilities();
      check(caps["commands"].size() == 31, "missing upstream commands");
      for (const auto &command : caps["commands"])
        if (command["name"] == "save_word_note" &&
            command["available"] == false)
          check(call(app, "save_word_note",
                     {{"wordId", wordId},
                      {"bookmarked", true},
                      {"note", "private"}})["error"]["code"] ==
                    "NOT_IMPLEMENTED",
                "unimplemented write reported success");
      app.database().withConnection([&](sqlite3 *db) {
        p::Statement fts(db, "SELECT COUNT(*) FROM words_fts WHERE words_fts "
                             "MATCH 'word:meticulous'");
        fts.step();
        check(fts.integer(0) > 0, "FTS triggers not working");
        try {
          p::Transaction tx(db);
          p::execute(db,
                     "INSERT INTO word_notes(word_id,note,updated_at) VALUES(" +
                         std::to_string(wordId) +
                         ",'rollback','2026-10-21T00:00:00.000Z')");
          throw std::runtime_error("injected");
        } catch (const std::runtime_error &) {
        }
        p::Statement notes(db, "SELECT COUNT(*) FROM word_notes");
        notes.step();
        check(notes.integer(0) == 0, "transaction did not roll back");
        p::Statement events(db, "SELECT COUNT(*) FROM encounters");
        events.step();
        check(!readOnlySeed || events.integer(0) == 0,
              "Phase 0 query wrote fake encounters");
        p::Statement fk(db, "PRAGMA foreign_keys");
        fk.step();
        check(fk.integer(0) == 1, "foreign keys disabled");
      });
    }
    {
      p::Application app(file, &clock);
      check(call(app, "app_info")["result"]["word_count"] == 55,
            "seed import not idempotent");
      check(
          call(app, "lookup_word",
               {{"word", "meticulous"}})["result"]["outcome"]["entry"]["id"] ==
              wordId,
          "stable id changed after reopen");
    }
    // Open an actual v2 database, then migrate it while retaining user facts.
    const auto upgrade = dir / L"upgrade.db";
    sqlite3 *raw = nullptr;
    check(sqlite3_open(upgrade.u8string().c_str(), &raw) == SQLITE_OK,
          "open upgrade db");
    for (const auto *name : {"0001_init.sql", "0002_freq_rank_index.sql"}) {
      std::ifstream input(std::filesystem::u8path(LEXICA_SOURCE_DIR) /
                          "src/parity/storage/migrations" / name);
      p::execute(raw, std::string(std::istreambuf_iterator<char>(input), {}));
    }
    p::execute(raw, "PRAGMA user_version=2;INSERT INTO "
                    "words(id,word,display,created_at)VALUES(900,'retained','"
                    "retained','2026-10-21T00:00:00.000Z');INSERT INTO "
                    "encounters(word_id,visited_at,comprehension_level)VALUES("
                    "900,'2026-10-21T00:00:00.000Z','english')");
    sqlite3_close(raw);
    {
      p::Database db(upgrade);
      db.withConnection([](sqlite3 *sql) {
        p::Statement q(
            sql,
            "SELECT comprehension_level FROM encounters WHERE word_id=900");
        check(q.step() && q.text(0) == "english", "migration lost events");
      });
    }
    const auto future = dir / L"future.db";
    sqlite3_open(future.u8string().c_str(), &raw);
    p::execute(raw, "PRAGMA user_version=999");
    sqlite3_close(raw);
    bool rejected = false;
    try {
      p::Database db(future);
    } catch (const p::Error &e) {
      rejected = e.code == "VERSION_TOO_NEW";
    }
    check(rejected, "newer DB was accepted");
    const auto legacy = dir / L"legacy.db";
    sqlite3_open(legacy.u8string().c_str(), &raw);
    p::execute(raw, "CREATE TABLE dictionary_entries(id INTEGER)");
    sqlite3_close(raw);
    rejected = false;
    try {
      p::Database db(legacy);
    } catch (const p::Error &e) {
      rejected = e.code == "SCHEMA_MISMATCH";
    }
    check(rejected, "legacy DB was overwritten");
    const auto now = clock.nowMillis();
    check(p::parseUtc(p::formatUtc(now)) == now, "UTC millisecond roundtrip");
    rejected = false;
    try {
      p::parseUtc("2026-02-30T00:00:00.000Z");
    } catch (const p::Error &) {
      rejected = true;
    }
    check(rejected, "invalid calendar date accepted");
    std::cout << "Parity foundation: schema, rollback, seed, router, DTO, "
                 "protocol and migration checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
