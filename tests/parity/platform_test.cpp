#include "parity/api/events.h"
#include "parity/app/application.h"
#include "parity/app/executor.h"
#include "parity/app/paths.h"
#include "parity/storage/legacy.h"
#include "parity/storage/upstream.h"
#include "test_support.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <thread>
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
  return app.dispatch({{"protocol_version", 1},
                       {"request_id", "platform-" + std::to_string(++sequence)},
                       {"command", command},
                       {"args", std::move(args)}});
}
void writeSql(sqlite3 *db, const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  p::execute(db, std::string(std::istreambuf_iterator<char>(input), {}));
}
int main() {
  try {
    check(p::schemaVersion() == 6, "schema version drifted");
    check(std::string(p::kCredentialService) == "LexicaReproduce.parity-v1" &&
              std::string(p::kCredentialAccount) == "ai_api_key" &&
              std::string(p::kCredentialService) != "com.lexica.app",
          "credential identity collides with the original app");
    const auto lanes = p::backgroundCommands();
    std::string listed;
    for (const auto &name : lanes)
      listed += name + " ";
    check(std::find(lanes.begin(), lanes.end(), "ensure_dictionary") !=
                  lanes.end() &&
              std::find(lanes.begin(), lanes.end(), "import_ecdict") !=
                  lanes.end() &&
              std::find(lanes.begin(), lanes.end(), "lookup_word") ==
                  lanes.end(),
          "background lanes are wrong: [" + listed + "]");
    const auto dir = test_support::freshDirectory("parity", "platform");
    const auto root = dir / L"应用根" / L"中文";
    auto layout = p::resolveLayout(root);
    check(layout.profile.filename() == L"webview-profile" &&
              std::filesystem::equivalent(layout.profile.parent_path(),
                                          layout.root),
          "profile followed the database");
    check(std::filesystem::is_regular_file(layout.root / L"db_dir.txt"),
          "db_dir.txt was not created");
    const auto relocated = dir / L"词库位置";
    {
      std::ofstream pointer(layout.root / L"db_dir.txt", std::ios::binary);
      pointer << relocated.u8string();
    }
    layout = p::resolveLayout(root);
    check(std::filesystem::equivalent(layout.databaseDirectory, relocated),
          "db_dir.txt was ignored");
    check(layout.profile.parent_path() == layout.root,
          "profile left the app root");
    {
      std::ofstream pointer(layout.root / L"db_dir.txt", std::ios::binary);
      pointer << "relative/db";
    }
    bool rejected = false;
    try {
      p::resolveLayout(root);
    } catch (const p::Error &error) {
      rejected = error.code == "DATA_DIRECTORY_ERROR";
    }
    check(rejected, "relative db_dir.txt was accepted");
    const auto original = p::originalDatabaseFile();
    if (!original.empty()) {
      const auto before = std::filesystem::file_size(original);
      {
        std::ofstream pointer(layout.root / L"db_dir.txt", std::ios::binary);
        pointer << original.parent_path().u8string();
      }
      rejected = false;
      try {
        p::resolveLayout(root);
      } catch (const p::Error &error) {
        rejected = error.code == "DATA_DIRECTORY_ERROR";
      }
      check(rejected, "original Lexica directory was accepted");
      check(std::filesystem::file_size(original) == before,
            "original database size changed");
      const auto probe = p::probeUpstreamDatabase(original);
      std::ofstream report(dir / L"upstream-probe.json", std::ios::binary);
      report << probe.dump(2);
      check(probe["opened_readonly"] == true && probe["mutated"] == false,
            "upstream probe wrote the original database");
      const auto version = probe["user_version"].get<int>();
      check(version >= 1 && version <= 5,
            "real upstream database version is unusable");
      check(probe["sample_word"] == "meticulous",
            "real upstream lookup failed");
      if (version == 5)
        check(probe["tables_ok"] == true,
              "version 5 database is missing tables");
      if (version == 4) {
        check(probe["missing"].size() == 2 &&
                  probe["missing"][0] == "word_enrichments" &&
                  probe["missing"][1] == "custom_wordbook_words",
              "version 4 database is not one migration behind");
      }
      std::cout << "upstream-probe user_version=" << probe["user_version"]
                << " bytes=" << probe["bytes"] << " mutated=false\n";
    } else {
      std::cout << "upstream-probe skipped: no original database\n";
    }
    const auto synthetic = dir / L"synthetic.db";
    sqlite3 *raw = nullptr;
    check(sqlite3_open(synthetic.u8string().c_str(), &raw) == SQLITE_OK,
          "synthetic open failed");
    for (const auto *name :
         {"0001_init.sql", "0002_freq_rank_index.sql", "0003_word_notes.sql",
          "0004_ai_examples.sql", "0005_study_system.sql",
          "0006_lexicon_provenance.sql"})
      writeSql(raw, std::filesystem::u8path(LEXICA_SOURCE_DIR) /
                        "src/parity/storage/migrations" / name);
    p::execute(raw, "PRAGMA user_version=6;INSERT INTO "
                    "words(word,display,source,created_at) VALUES('zzcompat',"
                    "'zzcompat','ecdict','2026-10-21T00:00:00.000Z')");
    sqlite3_close(raw);
    {
      p::Application app(synthetic);
      const auto caps = app.capabilities();
      check(caps["contract_version"] == "1.2.0", "contract version mismatch");
      check(call(app, "lookup_word",
                 {{"word", "zzcompat"}})["result"]["outcome"]["type"] == "hit",
            "upstream-shaped row was lost");
      const auto info = call(app, "app_info");
      check(info["ok"] == true &&
                info["result"]["word_count"].get<std::int64_t>() == 56,
            "seed import disturbed the upstream row: " + info.dump());
    }
    const auto version4 = dir / L"version4.db";
    check(sqlite3_open(version4.u8string().c_str(), &raw) == SQLITE_OK,
          "v4 open");
    for (const auto *name : {"0001_init.sql", "0002_freq_rank_index.sql",
                             "0003_word_notes.sql", "0004_ai_examples.sql"})
      writeSql(raw, std::filesystem::u8path(LEXICA_SOURCE_DIR) /
                        "src/parity/storage/migrations" / name);
    p::execute(raw, "PRAGMA user_version=4;INSERT INTO "
                    "words(word,display,source,created_at) VALUES('kept',"
                    "'kept','ecdict','2026-10-21T00:00:00.000Z')");
    sqlite3_close(raw);
    {
      p::Database upgraded(version4);
      upgraded.withConnection([](sqlite3 *db) {
        p::Statement version(db, "PRAGMA user_version");
        version.step();
        check(version.integer(0) == 6, "version 4 was not appended to 6");
        p::Statement word(db, "SELECT source FROM words WHERE word='kept'");
        check(word.step() && word.text(0) == "ecdict",
              "version 4 row was lost");
        p::Statement table(
            db, "SELECT 1 FROM sqlite_master WHERE name='word_enrichments'");
        check(table.step(), "migration 5 was not appended");
        p::Statement column(db, "SELECT 1 FROM pragma_table_info('words') "
                               "WHERE name='frequency_source'");
        check(column.step(), "migration 6 provenance column missing");
      });
    }
    {
      p::Json seen;
      p::Application app(dir / L"events.db", nullptr,
                         [&](const std::string &name, const p::Json &payload) {
                           seen = {{"event", name}, {"payload", payload}};
                         });
      app.emit("dictionary-setup", {{"phase", "downloading"},
                                    {"progress", 0.25},
                                    {"message", "Downloading…"}});
      check(seen["event"] == "dictionary-setup" &&
                seen["payload"]["phase"] == "downloading" &&
                seen["payload"]["progress"] == 0.25,
            "dictionary-setup event was dropped");
    }
    const auto legacy = dir / L"legacy.db";
    check(sqlite3_open(legacy.u8string().c_str(), &raw) == SQLITE_OK,
          "legacy open");
    p::execute(
        raw,
        "CREATE TABLE schema_migrations(id INTEGER PRIMARY KEY, module "
        "TEXT, version INTEGER, name TEXT);"
        "INSERT INTO schema_migrations(id,module,version,name) "
        "VALUES(1,'dict',2,'create dictionary tables');"
        "CREATE TABLE dictionary_entries(id INTEGER PRIMARY KEY, word "
        "TEXT, phonetic TEXT, translation TEXT, definition TEXT, raw TEXT);"
        "CREATE TABLE review_state(entry_id INTEGER PRIMARY KEY, stage "
        "INTEGER, due_utc INTEGER);"
        "INSERT INTO review_state(entry_id,stage,due_utc) VALUES(1,3,10);"
        "CREATE TABLE vocab_item(entry_id INTEGER PRIMARY KEY, word TEXT, "
        "favorite INTEGER, note TEXT, paused INTEGER);"
        "INSERT INTO vocab_item(entry_id,word,favorite,note,paused) "
        "VALUES(1,'Meticulous',1,'legacy note',1),"
        "(2,'not-in-dictionary',1,'skip',0);"
        "CREATE TABLE vocab_history(seq INTEGER PRIMARY KEY, entry_id "
        "INTEGER, utc INTEGER, kind TEXT, qid INTEGER);"
        "INSERT INTO vocab_history(entry_id,utc,kind) VALUES(1,10,'lookup')");
    sqlite3_close(raw);
    const auto legacyBytes = std::filesystem::file_size(legacy);
    FixedClock clock;
    p::Application target(dir / L"migrated.db", &clock);
    const auto wordId = target.database().withConnection([](sqlite3 *db) {
      p::Statement word(db, "SELECT id FROM words WHERE word='meticulous'");
      check(word.step(), "seed word missing");
      return word.integer(0);
    });
    const auto migrated = p::migrateLegacy(legacy, target.database(), clock);
    check(migrated["source_modified"] == false, "legacy source was modified");
    check(std::filesystem::file_size(legacy) == legacyBytes,
          "legacy bytes changed");
    check(migrated["copied_notes"] == 1 && migrated["copied_ignored"] == 1 &&
              migrated["skipped_missing_word"] == 1,
          "legacy personal rows were not mapped");
    target.database().withConnection([&](sqlite3 *db) {
      p::Statement note(
          db, "SELECT bookmarked,note FROM word_notes WHERE word_id=?");
      note.bind(1, wordId);
      check(note.step() && note.integer(0) == 1 &&
                note.text(1) == "legacy note",
            "legacy note missing");
      p::Statement ignored(
          db, "SELECT COUNT(*) FROM ignored_words WHERE word_id=?");
      ignored.bind(1, wordId);
      ignored.step();
      check(ignored.integer(0) == 1, "legacy pause missing");
      p::Statement memory(db, "SELECT COUNT(*) FROM memory_states");
      memory.step();
      p::Statement encounters(db, "SELECT COUNT(*) FROM encounters");
      encounters.step();
      check(memory.integer(0) == 0 && encounters.integer(0) == 0,
            "legacy history became fake learning events");
    });
    std::ofstream migrationReport(dir / L"legacy-report.json",
                                  std::ios::binary);
    migrationReport << migrated.dump(2);
    std::atomic<bool> holding{false};
    std::thread side([&] {
      target.database().withSideConnection([&](sqlite3 *) {
        holding = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
      });
    });
    while (!holding)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto started = std::chrono::steady_clock::now();
    check(call(target, "lookup_word", {{"word", "meticulous"}})["ok"] == true,
          "lookup failed while a side connection was held");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    side.join();
    check(elapsed < 200, "lookup waited on the side connection");
    p::EventHub hub;
    auto cursor = hub.tip();
    hub.publish("dictionary-setup", {{"phase", "extracting"},
                                     {"progress", nullptr},
                                     {"message", "Extracting…"}});
    const auto chunk = hub.waitAfter(cursor, 1000, nullptr);
    check(chunk && chunk->find("dictionary-setup") != std::string::npos &&
              chunk->find("extracting") != std::string::npos,
          "event hub dropped dictionary-setup");
    std::atomic<bool> backgroundRunning{false};
    std::atomic<bool> interactiveDone{false};
    p::Executor executor(
        [&](const p::Json &request, const p::Cancellation *cancel) {
          if (request.value("command", std::string{}) == "import_ecdict") {
            backgroundRunning = true;
            while (cancel && !cancel->requested())
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return p::Json{
                {"ok", false},
                {"error",
                 {{"code", "CANCELLED"}, {"message", "Command cancelled"}}}};
          }
          interactiveDone = true;
          return p::Json{{"ok", true}, {"result", nullptr}};
        });
    executor.submit({{"protocol_version", 1},
                     {"request_id", "slow"},
                     {"command", "import_ecdict"},
                     {"args", {{"sourcePath", "x"}}}},
                    [](p::Json) {});
    for (int i = 0; i < 200 && !backgroundRunning; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(backgroundRunning, "background command did not start");
    const auto lookupStart = std::chrono::steady_clock::now();
    std::atomic<bool> lookupDone{false};
    executor.submit({{"protocol_version", 1},
                     {"request_id", "fast"},
                     {"command", "lookup_word"},
                     {"args", {{"word", "meticulous"}}}},
                    [&](p::Json reply) {
                      lookupDone = reply["ok"] == true;
                      interactiveDone = true;
                    });
    for (int i = 0; i < 100 && !lookupDone; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto lookupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - lookupStart)
                              .count();
    check(lookupDone && lookupMs < 500,
          "lookup queued behind the background command");
    executor.shutdown();
    std::cout << "Parity platform: layout, upstream probe, legacy migration, "
                 "events and lanes passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
