#include "parity/app/application.h"
#include "parity/modules/dictionary/api.h"
#include "parity/modules/dictionary/normalize.h"
#include "parity/modules/dictionary/roots.h"
#include "test_support.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <intrin.h>
#include <iostream>
#include <iterator>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
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
       {"request_id", "dictionary-" + std::to_string(++sequence)},
       {"command", command},
       {"args", std::move(args)}});
}
void runProcess(const std::wstring &command, const std::filesystem::path &cwd) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring mutableCommand = command;
  if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr,
                      cwd.empty() ? nullptr : cwd.c_str(), &startup, &process))
    throw std::runtime_error("Cannot start process");
  WaitForSingleObject(process.hProcess, 60000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  if (code != 0)
    throw std::runtime_error("Process failed");
}
struct Sqlite {
  sqlite3 *db = nullptr;
  explicit Sqlite(const std::filesystem::path &path) {
    check(sqlite3_open(path.u8string().c_str(), &db) == SQLITE_OK,
          "open sqlite");
  }
  ~Sqlite() {
    if (db)
      sqlite3_close(db);
  }
};
void sql(sqlite3 *db, const std::string &text) { p::execute(db, text); }
std::int64_t scalar(sqlite3 *db, const std::string &text) {
  p::Statement query(db, text);
  query.step();
  return query.integer(0);
}
p::WordEntry hit(p::Application &app, const std::string &word) {
  return app.database().withConnection([&](sqlite3 *db) {
    auto entry = p::dictionaryFind(db, word);
    check(entry.has_value(), word);
    return *entry;
  });
}
struct ZipServer {
  SOCKET listen = INVALID_SOCKET;
  std::thread worker;
  int port = 0;
  std::string body;
  std::atomic<bool> stop{false};
  explicit ZipServer(std::string payload) : body(std::move(payload)) {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listen, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0,
          "bind");
    check(::listen(listen, 4) == 0, "listen");
    int length = sizeof(address);
    getsockname(listen, reinterpret_cast<sockaddr *>(&address), &length);
    port = ntohs(address.sin_port);
    worker = std::thread([this] { serve(); });
  }
  void serve() {
    while (!stop.load()) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(listen, &fds);
      timeval wait{0, 200000};
      if (select(0, &fds, nullptr, nullptr, &wait) <= 0)
        continue;
      SOCKET client = accept(listen, nullptr, nullptr);
      if (client == INVALID_SOCKET)
        continue;
      std::string request;
      char buffer[1024];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const int n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0)
          break;
        request.append(buffer, buffer + n);
      }
      const auto header =
          "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nConnection: close\r\n\r\n";
      send(client, header.data(), static_cast<int>(header.size()), 0);
      std::size_t offset = 0;
      while (offset < body.size()) {
        const int n = send(client, body.data() + offset,
                           static_cast<int>(std::min<std::size_t>(
                               body.size() - offset, 65536)),
                           0);
        if (n <= 0)
          break;
        offset += static_cast<std::size_t>(n);
      }
      closesocket(client);
    }
  }
  ~ZipServer() {
    stop = true;
    if (listen != INVALID_SOCKET)
      closesocket(listen);
    if (worker.joinable())
      worker.join();
  }
};
std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
void insertStardict(sqlite3 *db, const std::string &values) {
  sql(db, "CREATE TABLE stardict (id INTEGER PRIMARY KEY, word TEXT, phonetic "
          "TEXT, definition TEXT, "
          "translation TEXT, pos TEXT, collins INTEGER, oxford INTEGER, tag "
          "TEXT, bnc INTEGER, "
          "frq INTEGER, exchange TEXT);");
  sql(db, "INSERT INTO stardict(word, phonetic, definition, translation, pos, "
          "collins, oxford, tag, "
          "bnc, frq, exchange) VALUES " +
              values);
}
int main() {
  try {
    check(p::normalizeWord("  Meticulous  ") == "meticulous", "case trim");
    check(p::normalizeWord("“meticulous”") == "meticulous",
          "curly double quotes");
    check(p::normalizeWord("don’t") == "don't", "curly apostrophe");
    check(p::normalizeWord("ｍｅｔｉｃｕｌｏｕｓ") == "meticulous",
          "fullwidth NFKC");
    check(p::normalizeWord("").empty() && p::normalizeWord("   ").empty(),
          "empty normalize");
    check(p::prefixUpperBound("met") == "meu" &&
              p::prefixUpperBound("a") == "b" &&
              p::prefixUpperBound("é") == "ê" &&
              p::prefixUpperBound("") == std::string("\xF4\x8F\xBF\xBF"),
          "prefix bound");
    check(p::affixRuleCount() >= 60, "rule count");
    const auto inevitable = p::decomposeRoots("inevitable");
    check(std::any_of(
              inevitable.begin(), inevitable.end(),
              [](const p::RootPart &part) { return part.part == "in-"; }) &&
              std::any_of(inevitable.begin(), inevitable.end(),
                          [](const p::RootPart &part) {
                            return part.part == "evit";
                          }) &&
              std::any_of(
                  inevitable.begin(), inevitable.end(),
                  [](const p::RootPart &part) { return part.part == "-able"; }),
          "decompose inevitable");
    const auto surgeryRoots = p::decomposeRoots("surgery");
    check(std::any_of(
              surgeryRoots.begin(), surgeryRoots.end(),
              [](const p::RootPart &part) { return part.part == "surg"; }) &&
              std::any_of(
                  surgeryRoots.begin(), surgeryRoots.end(),
                  [](const p::RootPart &part) { return part.part == "-ery"; }),
          "decompose surgery");
    check(p::levenshtein("surgery", "surgery") == 0 &&
              p::levenshtein("surgery", "sugary") == 2 &&
              p::levenshtein("surgery", "surely") == 2 &&
              p::levenshtein("surgery", "surety") == 2 &&
              p::levenshtein("surgery", "surge") == 2 &&
              p::levenshtein("meticulous", "meticulus") == 1,
          "levenshtein");
    const auto parsed = p::parseWordFamilyEntry("microsurgery n. 显微外科");
    check(std::get<0>(parsed) == "microsurgery" &&
              std::get<1>(parsed) == "n." && std::get<2>(parsed) == "显微外科",
          "family parse");
    const auto parsed2 = p::parseWordFamilyEntry("meticulously adv.");
    check(std::get<0>(parsed2) == "meticulously" &&
              std::get<1>(parsed2) == "adv." && !std::get<2>(parsed2),
          "family parse without gloss");

    const auto root = test_support::freshDirectory("parity", "dictionary");
    FixedClock clock;
    const auto recreatedPath = root / "recreated.db";
    for (int generation = 0; generation < 2; ++generation) {
      {
        p::Application recreated(recreatedPath, &clock);
        recreated.database().withConnection([](sqlite3 *db) {
          check(scalar(db, "SELECT COUNT(*) FROM word_enrichments WHERE mnemonic IS NOT NULL") > 0,
                "seed metadata missing before app_info or after recreating a database");
          const auto changes = sqlite3_total_changes(db);
          std::string touched;
          sqlite3_update_hook(db, [](void *out, int, const char *, const char *table, sqlite3_int64 id) {
            *static_cast<std::string *>(out) += std::string(table) + ":" + std::to_string(id) + " ";
          }, &touched);
          p::backfillSeedMetadata(db, "2026-10-30T00:00:00.000Z");
          sqlite3_update_hook(db, nullptr, nullptr);
          check(sqlite3_total_changes(db) == changes,
                "unchanged seed metadata must not create startup writes or trigger a WAL checkpoint: " + touched);
          return 0;
        });
      }
      std::filesystem::remove(recreatedPath);
    }
    std::vector<p::Json> events;
    std::int64_t meticulousId = 0;
    {
      p::Application app(root / "lexica.db", &clock,
                         [&](const std::string &event, const p::Json &payload) {
                           events.push_back(
                               {{"event", event}, {"payload", payload}});
                         });
      const auto info = call(app, "app_info");
      check(info["ok"] == true && info["result"]["schema_version"] == 6 &&
                info["result"]["word_count"] == 55 &&
                info["result"]["fts_ok"] == true &&
                info["result"]["dictionary_ready"] == false,
            "seed app_info");
      const auto caps = app.capabilities();
      check(caps["automatic_dictionary_setup"] == true, "ensure registration");
      for (const auto &command : caps["commands"]) {
        if (command["name"] == "search_suggest")
          check(command["available"] == true &&
                    command["implementation"] == "implemented",
                "suggest status");
        if (command["name"] == "ensure_dictionary" ||
            command["name"] == "import_ecdict")
          check(command["available"] == true &&
                    command["implementation"] == "implemented",
                "import status");
      }
      const auto meticulous = hit(app, "  “Meticulous”  ");
      meticulousId = meticulous.id;
      check(meticulous.word == "meticulous" && meticulous.senses.size() == 1 &&
                meticulous.senses[0].pos == "adj." &&
                meticulous.senses[0].english_definition.rfind("very careful",
                                                              0) == 0 &&
                meticulous.senses[0].examples.size() == 3 &&
                meticulous.senses[0].examples[0].translation &&
                meticulous.collocations.size() == 2 &&
                meticulous.synonyms.size() == 4 &&
                meticulous.antonyms.size() == 3 &&
                meticulous.word_family.size() == 2,
            "meticulous entry");
      const auto elaborate = hit(app, "elaborate");
      check(elaborate.senses.size() == 2 && elaborate.senses[0].pos == "adj." &&
                elaborate.senses[1].pos == "v." &&
                elaborate.senses[1].examples.size() == 2,
            "sense order");
      check(hit(app, "deteriorated").word == "deteriorate" &&
                hit(app, "scrutinizing").word == "scrutinize",
            "inflection");
      const auto surgery = hit(app, "surgery");
      check(surgery.phonetic == "/ˈsɜːdʒəri/" && surgery.oxford &&
                surgery.senses.size() == 4 && surgery.senses[0].pos == "noun" &&
                surgery.senses[0].level == "B2" &&
                surgery.senses[0].level_source == "seed" &&
                surgery.senses[1].level == "B2" &&
                !surgery.senses[2].level && !surgery.senses[2].level_source &&
                !surgery.senses[3].level && surgery.forms.size() == 1 &&
                surgery.forms[0].label == "Plural" &&
                surgery.forms[0].form == "surgeries" && surgery.exam_example &&
                surgery.derivatives.size() == 1 &&
                surgery.derivatives[0].word == "microsurgery" &&
                surgery.derivatives[0].pos == "n." &&
                surgery.derivatives[0].gloss == "显微外科",
            "surgery content");
      check(surgery.tags ==
                std::vector<std::string>(
                    {"cet4", "cet6", "ky", "ielts", "toefl", "oxford"}),
            "surgery tags");
      check(surgery.mnemonic &&
                surgery.mnemonic->find("cheirourgia") != std::string::npos &&
                surgery.roots.size() == 2 && surgery.roots[0].part == "surg" &&
                surgery.roots[1].part == "-ory²",
            "seed roots stay distinct from derived roots");
      const auto look = surgery.lookalikes;
      check(std::any_of(look.begin(), look.end(),
                        [](const p::RelatedWordItem &item) {
                          return item.word == "sugary";
                        }) &&
                std::any_of(look.begin(), look.end(),
                            [](const p::RelatedWordItem &item) {
                              return item.word == "surely";
                            }) &&
                std::any_of(look.begin(), look.end(),
                            [](const p::RelatedWordItem &item) {
                              return item.word == "surety";
                            }),
            "lookalikes");
      const auto suggestions = call(app, "search_suggest", {{"query", "co"}});
      std::vector<std::string> words;
      for (const auto &item : suggestions["result"])
        words.push_back(item["word"]);
      const auto pos = [&](const char *word) {
        return std::find(words.begin(), words.end(), word) - words.begin();
      };
      check(pos("compelling") < pos("coherent") &&
                pos("coherent") < pos("conspicuous"),
            "frequency order");
      check(call(app, "search_suggest",
                 {{"query", "subtle"}})["result"][0]["word"] == "subtle",
            "exact suggestion first");
      check(call(app, "search_suggest", {{"query", "   "}})["result"].empty(),
            "blank suggest");
      const auto miss = call(app, "lookup_word", {{"word", "meti"}});
      check(miss["result"]["outcome"]["type"] == "miss", "miss");
      bool saw = false;
      for (const auto &item : miss["result"]["outcome"]["suggestions"])
        if (item["word"] == "meticulous")
          saw = true;
      check(saw && call(app, "lookup_word",
                        {{"word", "zzzz"}})["result"]["outcome"]["suggestions"]
                       .empty(),
            "miss suggestions");
      app.database().withConnection([&](sqlite3 *) {});
      app.database().withConnection([&](sqlite3 *db) {
        sql(db, "INSERT INTO words(word,display,source,created_at) "
                "VALUES('café','café','user','2026-01-01T00:00:00.000Z')");
        sql(db,
            "INSERT INTO senses(word_id,english_definition) VALUES((SELECT id "
            "FROM words WHERE word='café'),'a place where coffee is sold')");
        sql(db, "INSERT INTO words(word,display,source,created_at) "
                "VALUES('zephyrque','zephyrque','user','2026-01-01T00:00:00."
                "000Z')");
        sql(db, "INSERT INTO senses(word_id,english_definition,order_idx) "
                "VALUES((SELECT id FROM words WHERE word='zephyrque'),'test "
                "sense',0)");
        sql(db, "INSERT INTO "
                "words(word,display,phonetic,frequency_rank,synonyms,antonyms,"
                "word_family,tags,collins,oxford,source,created_at) "
                "VALUES('nullcase','nullcase',NULL,NULL,'[]','[]','[]','',NULL,"
                "0,'ecdict','2026-01-01T00:00:00.000Z')");
        sql(db, "INSERT INTO senses(word_id,english_definition,order_idx) "
                "VALUES((SELECT id FROM words WHERE word='nullcase'),'opaque "
                "token',0)");
        sql(db, "INSERT INTO collocations(word_id,text,gloss,order_idx) "
                "VALUES((SELECT id FROM words WHERE word='nullcase'),'opaque "
                "token',NULL,0)");
      });
      check(hit(app, "cafe").word == "café" && hit(app, "CAFÉ").word == "café",
            "diacritics");
      const auto zephyr = call(app, "search_suggest", {{"query", "zephyr"}});
      bool foundZephyr = false;
      for (const auto &item : zephyr["result"])
        if (item["word"] == "zephyrque")
          foundZephyr = true;
      check(foundZephyr, "alpha fill");
      const auto missing = hit(app, "nullcase");
      check(!missing.phonetic && missing.senses.size() == 1 &&
                !missing.senses[0].chinese_definition &&
                missing.senses[0].examples.empty() && !missing.collins &&
                !missing.oxford && missing.collocations.size() == 1 &&
                !missing.collocations[0].gloss && !missing.exam_example,
            "nullable fields");
      app.database().withConnection([&](sqlite3 *db) {
        check(scalar(db,
                     "SELECT COUNT(*) FROM word_enrichments WHERE "
                     "word_id=(SELECT id FROM words WHERE word='nullcase')") ==
                  0,
              "derived content is not stored as lexicon content");
        sql(db, "INSERT INTO word_notes(word_id,bookmarked,note,updated_at) "
                "VALUES(" +
                    std::to_string(meticulousId) +
                    ",1,'keep-me','2026-10-21T00:00:00.000Z')");
        sql(db, "INSERT INTO "
                "encounters(word_id,visited_at,comprehension_level) VALUES(" +
                    std::to_string(meticulousId) +
                    ",'2026-10-21T00:00:00.000Z','unknown')");
      });
    }
    {
      p::Application app(root / "lexica.db", &clock);
      check(call(app, "app_info")["result"]["word_count"] == 58,
            "seed stays idempotent");
      check(hit(app, "meticulous").id == meticulousId, "stable id");
      app.database().withConnection([&](sqlite3 *db) {
        check(scalar(db,
                     "SELECT COUNT(*) FROM word_notes WHERE note='keep-me'") ==
                  1,
              "note kept");
        check(scalar(db, "SELECT COUNT(*) FROM encounters") == 1,
              "encounter kept");
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE source='seed'") ==
                  55,
              "seed count");
      });
    }

    const auto sourcePath = root / "fixture.db";
    {
      Sqlite source(sourcePath);
      insertStardict(
          source.db,
          "('''a', NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL),"
          "('meticulous', 'mә''tikjulәs', 's. marked by precise accordance "
          "with details\n"
          "s. marked by extreme care', 'a. 一丝不苟的, 过细的', 'j:100', 1, 0, "
          "'toefl', 11166, 11107, ''),"
          "('go', 'gou', 'v. move away\nv. proceed', 'vi. 去\nvt. 进行', "
          "'v:100', 0, 1, 'cet4', 21, 18, "
          "'i:going/p:went/d:gone/3:goes/s:goes'),"
          "('emptyword', NULL, NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL),"
          "('a', NULL, 'art. one of a set', 'art. 一个', NULL, 0, 0, NULL, 5, "
          "5, NULL),"
          "('bank', NULL, 'n. a financial institution\nn. sloping land beside "
          "water', 'n. 银行\nn. 河岸', NULL, NULL, NULL, NULL, NULL, NULL, "
          "NULL),"
          "('fast', NULL, 'adj. moving quickly\nadv. firmly fixed', 'adj. "
          "快的', NULL, NULL, NULL, NULL, NULL, NULL, NULL),"
          "('inevitable hour', NULL, NULL, '死期', NULL, NULL, 0, NULL, 0, 0, "
          "NULL),"
          "('staronly', NULL, NULL, '星标词', NULL, 4, 0, NULL, 0, 0, NULL),"
          "('rankedword', NULL, 'n. ranked', 'n. 排名词', NULL, NULL, 0, NULL, "
          "4321, 1234, NULL),"
          "('bnconly', NULL, 'n. bnc', 'n. bnc 词', NULL, NULL, 0, NULL, "
          "8765, 0, NULL),"
          "('negrank', NULL, 'n. negative', 'n. 负值词', NULL, NULL, 0, NULL, "
          "-2, -5, NULL),"
          "('nullrank', NULL, 'n. null', 'n. 空值词', NULL, NULL, 0, NULL, "
          "NULL, NULL, NULL)");
    }
    {
      p::Application app(root / "lexica.db", &clock);
      const auto first =
          call(app, "import_ecdict", {{"sourcePath", sourcePath.u8string()}});
      check(first["ok"] == true && first["result"] == 10,
            "ecdict imported new rows");
      app.database().withConnection([&](sqlite3 *db) {
        check(scalar(db, "SELECT id FROM words WHERE word='meticulous'") ==
                  meticulousId,
              "seed wins");
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE word='meticulous' "
                         "AND source='seed'") == 1,
              "seed source");
        p::Statement row(db, "SELECT display, phonetic, frequency_rank, tags, "
                             "oxford FROM words WHERE word='go'");
        check(row.step(), "go row");
        check(row.text(0) == "go" && row.text(1) == "/gou/" &&
                  row.integer(2) == 18 && row.text(3) == "cet4" &&
                  row.integer(4) == 1,
              "go metadata");
        const auto goId = scalar(db, "SELECT id FROM words WHERE word='go'");
        p::Statement senses(
            db, "SELECT english_definition, chinese_definition, pos, level, "
                "level_source FROM senses WHERE word_id=? ORDER BY order_idx, id");
        senses.bind(1, goId);
        check(senses.step() && senses.text(0) == "v. move away" &&
                  senses.text(1) == "vi. 去" && senses.text(2) == "v." &&
                  senses.isNull(3) && senses.isNull(4),
              "paired sense has no invented CEFR level");
        check(senses.step() && senses.text(0) == "v. proceed" &&
                  senses.text(1) == "vt. 进行",
              "second sense");
        check(scalar(db, "SELECT word_id FROM word_forms WHERE form='went'") ==
                  goId,
              "past form");
        check(scalar(db, "SELECT COUNT(*) FROM word_forms WHERE form IN "
                         "('going','went','gone','goes') AND word_id=" +
                             std::to_string(goId)) == 4,
              "exchange forms");
        p::Statement kind(db, "SELECT kind FROM word_forms WHERE form='went'");
        check(kind.step() && kind.text(0) == "Past", "past label");
        p::Statement a(
            db, "SELECT display, frequency_rank FROM words WHERE word='a'");
        check(a.step() && a.text(0) == "a" && a.integer(1) == 5,
              "frequent homograph wins");
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE word='emptyword'") ==
                  0,
              "empty skipped");
        check(scalar(db, "SELECT COUNT(*) FROM senses s JOIN words w ON "
                         "w.id=s.word_id WHERE w.word='bank'") == 2,
              "bank senses");
        p::Statement fast(
            db, "SELECT english_definition, chinese_definition FROM senses s "
                "JOIN words w ON w.id=s.word_id WHERE w.word='fast'");
        check(fast.step() &&
                  fast.text(0) == "adj. moving quickly\nadv. firmly fixed" &&
                  fast.text(1) == "adj. 快的",
              "unbalanced lines collapse");
        const auto inevitable = *p::dictionaryFind(db, "inevitable hour");
        check(inevitable.display == "inevitable hour" &&
                  !inevitable.phonetic && !inevitable.frequency_rank &&
                  !inevitable.frequency_source && !inevitable.collins &&
                  !inevitable.oxford && inevitable.senses.size() == 1 &&
                  inevitable.senses[0].english_definition.empty() &&
                  inevitable.senses[0].chinese_definition &&
                  *inevitable.senses[0].chinese_definition == "死期" &&
                  inevitable.senses[0].examples.empty() &&
                  !inevitable.senses[0].level &&
                  !inevitable.senses[0].level_source,
              "inevitable hour keeps only the real Chinese gloss");
        const auto starred = *p::dictionaryFind(db, "staronly");
        check(!starred.frequency_rank && !starred.frequency_source &&
                  starred.collins && *starred.collins == 4 &&
                  !starred.senses[0].level && !starred.senses[0].level_source,
              "Collins stars never become frequency or CEFR");
        const auto ranked = *p::dictionaryFind(db, "rankedword");
        check(ranked.frequency_rank && *ranked.frequency_rank == 1234 &&
                  ranked.frequency_source &&
                  *ranked.frequency_source == "ecdict.frq" &&
                  !ranked.senses[0].level,
              "frq rank and source are recorded without a CEFR guess");
        const auto bncOnly = *p::dictionaryFind(db, "bnconly");
        check(bncOnly.frequency_rank && *bncOnly.frequency_rank == 8765 &&
                  bncOnly.frequency_source &&
                  *bncOnly.frequency_source == "ecdict.bnc",
              "bnc fallback records its own source");
        const auto negative = *p::dictionaryFind(db, "negrank");
        check(!negative.frequency_rank && !negative.frequency_source,
              "negative ranks are not treated as data");
        const auto nullRank = *p::dictionaryFind(db, "nullrank");
        check(!nullRank.frequency_rank && !nullRank.frequency_source,
              "null ranks are not treated as data");
        sql(db, "INSERT INTO words(word,display,frequency_rank,frequency_source,"
                "synonyms,antonyms,word_family,tags,collins,oxford,source,"
                "created_at) VALUES('legacyprobe','legacyprobe',4321,NULL,"
                "'[]','[]','[]','',NULL,0,'ecdict','2026-01-01T00:00:00.000Z')");
        sql(db, "INSERT INTO senses(word_id,english_definition,level,level_source,"
                "order_idx) VALUES((SELECT id FROM words WHERE "
                "word='legacyprobe'),'old data','B2',NULL,0)");
        sql(db, "INSERT INTO senses(word_id,english_definition,level,level_source,"
                "order_idx) VALUES((SELECT id FROM words WHERE "
                "word='legacyprobe'),'old rare','R',NULL,1)");
        check(scalar(db,
                     "SELECT COUNT(*) FROM word_notes WHERE note='keep-me'") ==
                  1,
              "import kept note");
      });
      const auto legacy = hit(app, "legacyprobe");
      check(legacy.frequency_rank && *legacy.frequency_rank == 4321 &&
                !legacy.frequency_source && legacy.senses.size() == 2 &&
                legacy.senses[0].level == "B2" &&
                legacy.senses[0].level_source == "legacy_unverified" &&
                !legacy.senses[1].level && !legacy.senses[1].level_source,
            "legacy levels stay conservative and R is not CEFR");
      const auto again =
          call(app, "import_ecdict", {{"sourcePath", sourcePath.u8string()}});
      check(again["ok"] == true && again["result"] == 0, "idempotent import");
      check(hit(app, "meticulous").id == meticulousId, "id after reimport");
    }
    {
      const auto bad = root / "bad.db";
      Sqlite empty(bad);
      sql(empty.db, "CREATE TABLE other(id INTEGER)");
      p::Application app(root / "lexica.db", &clock);
      const auto response =
          call(app, "import_ecdict", {{"sourcePath", bad.u8string()}});
      check(response["ok"] == false &&
                response["error"]["code"] == "RESOURCE_ERROR",
            "bad source");
      app.database().withConnection([&](sqlite3 *db) {
        check(scalar(db, "SELECT COUNT(*) FROM encounters") == 1,
              "failed import kept encounters");
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE word='go'") == 1,
              "failed import kept rows");
      });
    }
    {
      const auto batchPath = root / "batch.db";
      {
        Sqlite source(batchPath);
        sql(source.db, "CREATE TABLE stardict (id INTEGER PRIMARY KEY, word "
                       "TEXT, phonetic TEXT, definition TEXT, translation "
                       "TEXT, pos TEXT, collins INTEGER, oxford INTEGER, tag "
                       "TEXT, bnc INTEGER, frq INTEGER, exchange TEXT)");
        sqlite3_stmt *insert = nullptr;
        check(sqlite3_prepare_v2(
                  source.db,
                  "INSERT INTO stardict(word, definition) VALUES(?,'n. item')",
                  -1, &insert, nullptr) == SQLITE_OK,
              "prepare batch");
        p::execute(source.db, "BEGIN");
        for (int i = 0; i < 25; ++i) {
          const auto name = "batch" + std::to_string(i);
          sqlite3_bind_text(insert, 1, name.c_str(), -1, SQLITE_TRANSIENT);
          check(sqlite3_step(insert) == SQLITE_DONE, "insert batch");
          sqlite3_reset(insert);
        }
        sqlite3_finalize(insert);
        p::execute(source.db, "COMMIT");
      }
      p::Application app(root / "lexica.db", &clock);
      const auto before = app.database().withConnection([&](sqlite3 *db) {
        return scalar(db, "SELECT COUNT(*) FROM words");
      });
      int calls = 0;
      bool wrote = false;
      try {
        auto connection = p::openConfiguredDatabase(root / "lexica.db");
        p::EcdictImportHooks hooks;
        hooks.batchSize = 10;
        hooks.beforeCommit = [&] {
          if (++calls == 2)
            throw p::Error("INTERNAL_ERROR", "interrupted");
        };
        hooks.betweenBatches = [&] {
          app.database().withConnection([&](sqlite3 *db) {
            p::Statement note(
                db, "UPDATE word_notes SET note='during' WHERE word_id=?");
            note.bind(1, meticulousId).step();
            check(p::dictionaryFind(db, "meticulous").has_value(),
                  "query during import");
          });
          wrote = true;
        };
        p::importEcdictDatabase(connection.get(), batchPath,
                                "2026-10-21T00:00:00.000Z", hooks);
        check(false, "interrupt did not throw");
      } catch (const p::Error &error) {
        check(error.code == "INTERNAL_ERROR", "interrupt error");
      }
      check(wrote, "between-batch write ran");
      app.database().withConnection([&](sqlite3 *db) {
        check(scalar(db, "SELECT COUNT(*) FROM words") == before + 10,
              "partial batch kept");
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE word='batch20'") ==
                  0,
              "rolled back tail");
        check(
            scalar(db, "SELECT COUNT(*) FROM word_notes WHERE note='during'") ==
                1,
            "concurrent note");
        check(scalar(db, "SELECT COUNT(*) FROM encounters") == 1,
              "learning data kept");
      });
    }

    // A count-only 20,000-row batch can block learning writes for seconds on
    // slow machines. Force slow inserts and verify time-bounded commits allow
    // a real lookup (including its encounter write) before import completes.
    {
      p::Application app(root / "slow-import.db", &clock);
      auto connection = p::openConfiguredDatabase(app.database().path());
      check(sqlite3_create_function(
                connection.get(), "slow_import", 0, SQLITE_UTF8, nullptr,
                [](sqlite3_context *context, int, sqlite3_value **) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(20));
                  sqlite3_result_null(context);
                }, nullptr, nullptr) == SQLITE_OK,
            "register slow import function");
      p::execute(connection.get(),
                 "CREATE TEMP TRIGGER slow_import_row AFTER INSERT ON words "
                 "WHEN NEW.source='ecdict' BEGIN SELECT slow_import(); END");
      p::EcdictImportHooks hooks;
      int foregroundWrites = 0;
      hooks.betweenBatches = [&] {
        const auto lookup = call(app, "lookup_word", {{"word", "meticulous"}});
        check(lookup["ok"] == true &&
                  lookup["result"]["outcome"]["type"] == "hit",
              "foreground learning write blocked by import transaction");
        ++foregroundWrites;
      };
      check(p::importEcdictDatabase(connection.get(), root / "batch.db",
                                   "2026-10-21T00:00:00.000Z", hooks) == 25,
            "slow import lost rows");
      check(foregroundWrites >= 1,
            "slow import kept the writer until its count limit");
    }

    const auto ensureDir = test_support::freshDirectory("parity", "ensure");
    {
      const auto pack = ensureDir / "pack";
      std::filesystem::create_directories(pack);
      {
        Sqlite source(pack / "stardict.db");
        insertStardict(source.db,
                       "('downloaded', 'abc', 'n. fetched', 'n. 已下载', "
                       "'n:100', 0, 0, 'cet4', 10, 10, NULL)");
      }
      runProcess(
          L"C:\\Windows\\System32\\tar.exe -a -cf dictionary.zip stardict.db",
          pack);
      ZipServer server(readFile(pack / "dictionary.zip"));
      p::Database database(ensureDir / "lexica.db");
      database.seed();
      auto dictionary = p::makeDictionary();
      std::vector<std::string> phases;
      p::Context context{database, clock, *dictionary,
                         [&](const std::string &, const p::Json &payload) {
                           phases.push_back(payload["phase"]);
                         },
                         "ensure"};
      p::EnsureOptions options;
      options.urls = {"http://127.0.0.1:" + std::to_string(server.port) +
                      "/dictionary.zip"};
      const auto result = p::ensureDictionary(context, options);
      check(result.imported == 1 && result.ready == false &&
                result.skipped == false,
            "downloaded import");
      check(!phases.empty() && phases.front() == "checking" &&
                std::find(phases.begin(), phases.end(), "downloading") !=
                    phases.end() &&
                std::find(phases.begin(), phases.end(), "extracting") !=
                    phases.end() &&
                std::find(phases.begin(), phases.end(), "importing") !=
                    phases.end() &&
                phases.back() == "ready",
            "setup phases");
      database.withConnection([&](sqlite3 *db) {
        check(scalar(db,
                     "SELECT COUNT(*) FROM words WHERE word='downloaded'") == 1,
              "downloaded word");
      });
    }
    {
      const auto badDir = test_support::freshDirectory("parity", "badzip");
      p::Application app(badDir / "lexica.db", &clock);
      {
        std::ofstream junk(badDir / "ecdict-sqlite-28.zip", std::ios::binary);
        std::string pad(1100000, 'x');
        junk.write(pad.data(), static_cast<std::streamsize>(pad.size()));
      }
      const auto response = call(app, "ensure_dictionary");
      check(response["ok"] == false, "bad archive fails");
      check(response["error"]["code"] == "RESOURCE_ERROR", "bad archive code");
      app.database().withConnection([&](sqlite3 *db) {
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE source='ecdict'") ==
                  0,
              "bad archive wrote nothing");
      });
    }

    const auto skipDir = test_support::freshDirectory("parity", "skip");
    {
      Sqlite source(skipDir / "stardict.db");
      insertStardict(source.db, "('localbig', NULL, 'n. local', 'n. 本地', "
                                "NULL, NULL, NULL, NULL, NULL, 3, NULL)");
      sql(source.db,
          "CREATE TABLE pad(blob); INSERT INTO pad VALUES(zeroblob(1100000));");
    }
    {
      std::ofstream junk(skipDir / "ecdict-sqlite-28.zip", std::ios::binary);
      std::string pad(1100000, 'z');
      junk.write(pad.data(), static_cast<std::streamsize>(pad.size()));
    }
    {
      p::Database database(skipDir / "lexica.db");
      database.seed();
      auto dictionary = p::makeDictionary();
      p::Context context{database, clock, *dictionary, {}, "skip"};
      p::EnsureOptions options;
      options.urls = {"http://127.0.0.1:1/do-not-download"};
      const auto result = p::ensureDictionary(context, options);
      check(result.imported == 1 && result.ready == false &&
                result.skipped == false,
            "local import skipped network");
      database.withConnection([&](sqlite3 *db) {
        check(scalar(db, "SELECT COUNT(*) FROM words WHERE word='localbig'") ==
                  1,
              "local word");
        sql(db,
            "INSERT INTO custom_wordbook_words(word_id, added_at) SELECT id, "
            "'2026-10-21T00:00:00.000Z' FROM words WHERE word='localbig'");
      });
      {
        std::ofstream junk(skipDir / "ecdict-sqlite-28.zip", std::ios::binary);
        std::string pad(1100000, 'z');
        junk.write(pad.data(), static_cast<std::streamsize>(pad.size()));
      }
      const auto again = p::ensureDictionary(context, options);
      check(again.imported == 0, "second local import is idempotent");
      database.withConnection([&](sqlite3 *db) {
        check(scalar(db, "SELECT COUNT(*) FROM custom_wordbook_words") == 1,
              "personal row kept");
      });
    }

    const auto perfDir = test_support::freshDirectory("parity", "perf");
    const auto perfSource = perfDir / "perf.db";
    constexpr int perfCount = 40000;
    {
      Sqlite source(perfSource);
      sql(source.db,
          "CREATE TABLE stardict (id INTEGER PRIMARY KEY, word TEXT, phonetic "
          "TEXT, definition TEXT, translation TEXT, pos TEXT, collins INTEGER, "
          "oxford INTEGER, tag TEXT, bnc INTEGER, frq INTEGER, exchange TEXT)");
      p::execute(source.db, "BEGIN");
      sqlite3_stmt *insert = nullptr;
      check(sqlite3_prepare_v2(source.db,
                               "INSERT INTO stardict(word, phonetic, "
                               "definition, translation, frq) "
                               "VALUES(?,'x','n. sample','n. 样本',?)",
                               -1, &insert, nullptr) == SQLITE_OK,
            "prepare perf");
      for (int i = 0; i < perfCount; ++i) {
        const auto name = "perf" + std::to_string(i);
        sqlite3_bind_text(insert, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert, 2, i + 1);
        check(sqlite3_step(insert) == SQLITE_DONE, "insert perf");
        sqlite3_reset(insert);
      }
      sqlite3_finalize(insert);
      p::execute(source.db, "COMMIT");
    }
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    int cpu[4] = {};
    __cpuid(cpu, 0x80000002);
    char brand[49] = {};
    std::memcpy(brand, cpu, sizeof(cpu));
    __cpuid(cpu, 0x80000003);
    std::memcpy(brand + 16, cpu, sizeof(cpu));
    __cpuid(cpu, 0x80000004);
    std::memcpy(brand + 32, cpu, sizeof(cpu));
    p::Application perfApp(perfDir / "lexica.db", &clock);
    const auto started = std::chrono::steady_clock::now();
    const auto imported =
        call(perfApp, "import_ecdict", {{"sourcePath", perfSource.u8string()}});
    const auto importMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    check(imported["ok"] == true && imported["result"] == perfCount,
          "perf import");
    std::int64_t warmFind = 0;
    std::int64_t warmSuggest = 0;
    perfApp.database().withConnection([&](sqlite3 *db) {
      const auto begin = std::chrono::steady_clock::now();
      check(p::dictionaryFind(db, "perf100")->word == "perf100", "perf find");
      warmFind = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - begin)
                     .count();
      const auto suggestBegin = std::chrono::steady_clock::now();
      auto rows = p::dictionarySuggest(db, "perf1", 8);
      check(!rows.empty() && rows.front().word == "perf1",
            "perf suggest exact");
      warmSuggest = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - suggestBegin)
                        .count();
    });
    std::int64_t coldFind = 0;
    std::int64_t coldSuggest = 0;
    std::int64_t hotFind = 0;
    std::int64_t hotSuggest = 0;
    {
      p::Application reopened(perfDir / "lexica.db", &clock);
      reopened.database().withConnection([&](sqlite3 *db) {
        const auto begin = std::chrono::steady_clock::now();
        check(p::dictionaryFind(db, "perf200")->word == "perf200", "cold find");
        coldFind = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - begin)
                       .count();
        const auto second = std::chrono::steady_clock::now();
        check(p::dictionaryFind(db, "perf200")->word == "perf200", "hot find");
        hotFind = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - second)
                      .count();
        const auto coldS = std::chrono::steady_clock::now();
        check(!p::dictionarySuggest(db, "perf2", 8).empty(), "cold suggest");
        coldSuggest = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - coldS)
                          .count();
        const auto hotS = std::chrono::steady_clock::now();
        check(!p::dictionarySuggest(db, "perf2", 8).empty(), "hot suggest");
        hotSuggest = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - hotS)
                         .count();
      });
    }
    std::ofstream report(perfDir / "perf.txt");
    report << "rows " << perfCount << "\nimport_ms " << importMs
           << "\nwarm_find_us " << warmFind << "\nwarm_suggest_us "
           << warmSuggest << "\ncold_connection_find_us " << coldFind
           << "\nhot_find_us " << hotFind << "\ncold_connection_suggest_us "
           << coldSuggest << "\nhot_suggest_us " << hotSuggest
           << "\nprocessors " << system.dwNumberOfProcessors << "\nram_mb "
           << (memory.ullTotalPhys / (1024 * 1024)) << "\ncpu " << brand
           << "\ncache os page cache warm after import; cold means new sqlite "
              "connection\n";
    std::cout << "PERF rows=" << perfCount << " import_ms=" << importMs
              << " cold_find_us=" << coldFind << " hot_find_us=" << hotFind
              << " cold_suggest_us=" << coldSuggest
              << " hot_suggest_us=" << hotSuggest << " cpu=" << brand
              << " ram_mb=" << (memory.ullTotalPhys / (1024 * 1024))
              << " processors=" << system.dwNumberOfProcessors << "\n";
    check(call(perfApp, "app_info")["result"]["dictionary_ready"] == false,
          "partial corpus is not ready");
    const auto markerDir = test_support::freshDirectory("parity", "marker");
    {
      p::Application marked(markerDir / "lexica.db", &clock);
      {
        std::ofstream marker(markerDir / "ecdict.ok", std::ios::binary);
        marker << "1";
      }
      const auto info = call(marked, "app_info");
      check(info["result"]["dictionary_ready"] == true &&
                info["result"]["word_count"] == 55,
            "completion marker reports the dictionary ready");
      const auto ensured = call(marked, "ensure_dictionary");
      check(ensured["ok"] == true && ensured["result"]["ready"] == true &&
                ensured["result"]["skipped"] == true &&
                ensured["result"]["imported"] == 0,
            "completion marker skips another download");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
