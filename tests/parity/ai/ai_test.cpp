#include "parity/app/application.h"
#include "parity/modules/ai/http.h"
#include "parity/modules/ai/keystore.h"
#include "parity/modules/ai/runtime.h"
#include "parity/modules/ai/text.h"
#include "test_support.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <wincred.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
namespace p = lexica::parity;
const char *kSecret = "sk-parity-TESTKEY-9f3a";
const char *kWinSecret = "sk-parity-WINCRED-42c7";
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
void noSecret(const std::string &text, const std::string &secret) {
  if (!secret.empty() && text.find(secret) != std::string::npos)
    throw std::runtime_error("response or log contains the API key");
}
struct FixedClock : p::Clock {
  p::UtcMillis nowMillis() const override { return 1792540800123LL; }
};
p::Json call(p::Application &app, const std::string &command,
             p::Json args = p::Json::object()) {
  static std::atomic<std::uint64_t> sequence{0};
  return app.dispatch({{"protocol_version", 1},
                       {"request_id", "ai-" + std::to_string(++sequence)},
                       {"command", command},
                       {"args", std::move(args)}});
}
struct MemoryKeys : p::KeyStore {
  std::mutex mu;
  std::optional<std::string> value;
  bool failSet = false;
  bool failGet = false;
  std::optional<std::string> get() override {
    std::lock_guard<std::mutex> lock(mu);
    if (failGet)
      throw p::Error("RESOURCE_ERROR", "credential store: unavailable");
    return value;
  }
  void set(const std::string &key) override {
    std::lock_guard<std::mutex> lock(mu);
    if (failSet)
      throw p::Error("RESOURCE_ERROR", "credential store: refused " + key);
    value = key;
  }
  void remove() override {
    std::lock_guard<std::mutex> lock(mu);
    value.reset();
  }
};
struct FakeHttp : p::HttpClient {
  std::mutex mu;
  std::condition_variable cv;
  int calls = 0;
  int inflight = 0;
  int maxInflight = 0;
  int releaseAt = 1;
  std::string mode = "mirror";
  std::string script;
  std::string lastUrl;
  int lastTimeout = 0;
  std::vector<std::string> bodies;
  bool bodyLeaked = false;
  p::Database *db = nullptr;
  bool watchLock = false;
  bool lockFree = false;
  std::thread probe;
  ~FakeHttp() { joinProbe(); }
  void joinProbe() {
    if (probe.joinable())
      probe.join();
  }
  static std::string wrap(const std::string &content) {
    p::Json body;
    body["choices"] = p::Json::array(
        {p::Json{{"message", {{"role", "assistant"}, {"content", content}}}}});
    return body.dump();
  }
  static std::string mirror(const std::string &body) {
    auto json = p::Json::parse(body);
    std::string system;
    std::string user;
    for (const auto &message : json.at("messages")) {
      if (message.at("role") == "system")
        system = message.at("content").get<std::string>();
      if (message.at("role") == "user")
        user = message.at("content").get<std::string>();
    }
    bool enrich = system.find("Enrich vocabulary") != std::string::npos;
    p::Json rows = p::Json::array();
    std::istringstream lines(user);
    std::string line;
    while (std::getline(lines, line)) {
      auto idAt = line.find("word_id: ");
      auto wordAt = line.find(" | word: ");
      if (idAt == std::string::npos || wordAt == std::string::npos)
        continue;
      auto id = std::stoll(line.substr(idAt + 9));
      auto word = line.substr(wordAt + 9);
      auto cut = word.find(" | ");
      if (cut != std::string::npos)
        word.resize(cut);
      if (enrich) {
        rows.push_back(
            {{"word_id", id},
             {"mnemonic", "助记"},
             {"roots",
              p::Json::array({p::Json{{"part", word}, {"meaning", "义"}}})},
             {"example", "The " + word + " stays visible in this sentence."},
             {"translation", "译文"}});
      } else {
        rows.push_back({{"word_id", id},
                        {"sense_id", nullptr},
                        {"sentence", "People call " + word + " " + word +
                                         " during the quiet afternoon."}});
      }
    }
    return rows.dump();
  }
  std::string postJson(const std::string &url, const std::string &bearer,
                       const std::string &body, int timeoutMs) override {
    std::string localMode;
    std::string localScript;
    std::string localBearer;
    {
      std::unique_lock<std::mutex> lock(mu);
      ++calls;
      ++inflight;
      maxInflight = std::max(maxInflight, inflight);
      lastUrl = url;
      lastTimeout = timeoutMs;
      bodies.push_back(body);
      if (!bearer.empty() && body.find(bearer) != std::string::npos)
        bodyLeaked = true;
      cv.notify_all();
      if (releaseAt > 1)
        cv.wait_for(lock, std::chrono::seconds(2),
                    [&] { return inflight >= releaseAt; });
      localMode = mode;
      localScript = script;
      localBearer = bearer;
    }
    struct Drop {
      FakeHttp *self;
      ~Drop() {
        std::lock_guard<std::mutex> lock(self->mu);
        --self->inflight;
        self->cv.notify_all();
      }
    } drop{this};
    if (watchLock && db) {
      auto flag = std::make_shared<std::atomic<bool>>(false);
      joinProbe();
      probe = std::thread([this, flag] {
        db->withConnection([&](sqlite3 *) {
          flag->store(true);
          return 0;
        });
      });
      auto start = std::chrono::steady_clock::now();
      while (!flag->load() && std::chrono::steady_clock::now() - start <
                                  std::chrono::milliseconds(250))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      lockFree = flag->load();
      if (lockFree)
        joinProbe();
    }
    if (localMode == "timeout")
      throw p::Error("NETWORK_ERROR", "AI request timed out");
    if (localMode == "fail")
      throw p::Error("NETWORK_ERROR", "AI request failed: connection refused");
    if (localMode == "leak")
      throw p::Error("NETWORK_ERROR", "AI request failed: " + localBearer);
    if (localMode == "bad-json")
      return "not json";
    if (localMode == "missing")
      return "{\"choices\":[]}";
    if (localMode == "script")
      return wrap(localScript);
    if (localMode == "plain")
      return localScript;
    try {
      return wrap(mirror(body));
    } catch (const std::exception &) {
      return wrap("[]");
    }
  }
};
struct RuntimeGuard {
  ~RuntimeGuard() { p::resetAiRuntime(); }
};
std::vector<std::string> splitKey(const std::string &key) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : key) {
    if (c == '\x1f') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}
void scanFile(const std::filesystem::path &path, const std::string &secret) {
  for (auto candidate : {path, std::filesystem::path(path.wstring() + L"-wal"),
                         std::filesystem::path(path.wstring() + L"-shm")}) {
    if (!std::filesystem::exists(candidate))
      continue;
    std::ifstream input(candidate, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    noSecret(bytes, secret);
  }
}
void insertWord(p::Application &app, std::int64_t id, const std::string &word,
                std::int64_t senseId, const std::string &english,
                const std::string &chinese, int order,
                const std::string &example) {
  app.database().withConnection([&](sqlite3 *db) {
    {
      p::Statement exists(db, "SELECT COUNT(*) FROM words WHERE id=?");
      exists.bind(1, id);
      exists.step();
      if (exists.integer(0) == 0) {
        p::Statement insert(
            db,
            "INSERT INTO words(id,word,display,created_at) VALUES(?,?,?,?)");
        insert.bind(1, id).bind(2, word).bind(3, word).bind(
            4, "2026-10-26T00:00:00.000Z");
        insert.step();
      }
    }
    p::Statement sense(
        db,
        "INSERT INTO "
        "senses(id,word_id,english_definition,chinese_definition,order_idx) "
        "VALUES(?,?,?,?,?)");
    sense.bind(1, senseId).bind(2, id).bind(3, english);
    if (chinese.empty())
      sense.null(4);
    else
      sense.bind(4, chinese);
    sense.bind(5, order);
    sense.step();
    if (!example.empty()) {
      p::Statement ex(
          db,
          "INSERT INTO examples(id,sense_id,text,order_idx) VALUES(?,?,?,0)");
      ex.bind(1, senseId + 10000).bind(2, senseId).bind(3, example);
      ex.step();
    }
    return 0;
  });
}
void testText() {
  check(p::normalizeWord("  Meticulous  ") == "meticulous", "normalize case");
  check(p::normalizeWord(u8"ｍｅｔｉｃｕｌｏｕｓ") == "meticulous",
        "normalize width");
  check(p::normalizeWord(u8"“meticulous”") == "meticulous", "normalize quotes");
  check(p::normalizeWord(u8"don’t") == "don't", "normalize apostrophe");
  p::GenWord word;
  word.word_id = 1;
  word.sense_id = 7;
  word.word = "meticulous";
  word.english = "very careful";
  auto good = "She was meticulous about checking every detail.";
  auto rows = p::validateAndBlank(
      std::string("[{\"word_id\":1,\"sense_id\":7,\"sentence\":\"") + good +
          "\"}]",
      {word});
  check(rows.size() == 1 &&
            rows[0].sentence == "She was ______ about checking every detail.",
        "case-sensitive blank failed");
  rows =
      p::validateAndBlank("[{\"word_id\":1,\"sense_id\":7,\"sentence\":\"She "
                          "was Meticulous about checking every detail.\"}]",
                          {word});
  check(rows.size() == 1 &&
            rows[0].sentence == "She was ______ about checking every detail.",
        "case fold blank failed");
  rows = p::validateAndBlank(
      "[{\"word_id\":1,\"sense_id\":7,\"sentence\":\"She was meticulous, "
      "always meticulous, about details.\"}]",
      {word});
  check(rows.size() == 1 &&
            rows[0].sentence == "She was ______, always ______, about details.",
        "repeated target blank failed");
  rows = p::validateAndBlank(
      "[{\"word_id\":1,\"sense_id\":7,\"sentence\":\"It meticulousness quickly "
      "showed during the review.\"},"
      "{\"word_id\":1,\"sense_id\":8,\"sentence\":\"She was meticulous about "
      "checking every detail.\"},"
      "{\"word_id\":999,\"sense_id\":7,\"sentence\":\"She was meticulous about "
      "checking every detail.\"},"
      "{\"word_id\":1,\"sense_id\":7,\"sentence\":\"   \"},"
      "{\"word_id\":1,\"sense_id\":7,\"sentence\":\"See meticulous.\"}]",
      {word});
  check(rows.empty(), "invalid example was accepted");
  check(p::parseJsonItems("```json\n[{\"word_id\":1}]\n```").size() == 1,
        "fence parse failed");
  check(p::parseJsonItems(
            "Here you go:\n[{\"word_id\":1},{\"word_id\":2}] hope it helps")
                .size() == 2,
        "prose parse failed");
  check(p::parseJsonItems("no json at all").empty(),
        "plain text parsed as items");
  std::string longSentence = "meticulous";
  for (int i = 0; i < 20; ++i)
    longSentence += " x";
  check(p::validateAndBlank("[{\"word_id\":1,\"sense_id\":7,\"sentence\":\"" +
                                longSentence + "\"}]",
                            {word})
            .empty(),
        "21-word sentence accepted");
}
struct Fixture {
  std::filesystem::path file;
  FixedClock clock;
  p::Application app;
  FakeHttp http;
  MemoryKeys keys;
  RuntimeGuard guard;
  explicit Fixture(const std::string &name)
      : file(test_support::freshDirectory("ai", name) / "lexica.db"),
        app(file, &clock) {
    http.db = &app.database();
    p::setAiRuntime(&keys, &http);
    p::clearAiTrace();
  }
  p::Json save(bool enabled, const std::string &base, const std::string &model,
               p::Json key) {
    return call(app, "ai_config_save",
                {{"enabled", enabled},
                 {"baseUrl", base},
                 {"model", model},
                 {"key", std::move(key)}});
  }
};
void testConfig() {
  Fixture fx("config");
  auto initial = call(fx.app, "ai_config_get");
  check(initial["ok"] == true, "config get failed");
  check(initial["result"]["enabled"] == false, "default enabled");
  check(initial["result"]["base_url"] == p::kDefaultAiBaseUrl, "default url");
  check(initial["result"]["model"] == p::kDefaultAiModel, "default model");
  check(initial["result"]["has_key"] == false, "default key");
  check(initial["result"].size() == 4, "config view has extra fields");
  fx.keys.failSet = true;
  auto refused = fx.save(true, "https://example.test/v1", "mock", kSecret);
  check(refused["ok"] == false && refused["error"]["code"] == "RESOURCE_ERROR",
        "credential failure was stored");
  noSecret(refused.dump(), kSecret);
  check(!fx.keys.value || *fx.keys.value != kSecret,
        "failed set stored the key");
  auto still = call(fx.app, "ai_config_get");
  check(still["result"]["enabled"] == false &&
            still["result"]["has_key"] == false,
        "failed save changed config");
  fx.keys.failSet = false;
  fx.keys.value = "old-key-value";
  auto kept =
      fx.save(true, "  https://example.test/v1/  ", "  mock-model  ", nullptr);
  check(kept["ok"] == true && kept["result"]["has_key"] == true,
        "null key did not keep");
  check(fx.keys.value == "old-key-value", "null key replaced the secret");
  check(kept["result"]["base_url"] == "https://example.test/v1/",
        "url was not trimmed");
  check(kept["result"]["model"] == "mock-model", "model was not trimmed");
  auto replaced =
      fx.save(true, "https://example.test/v1", "mock-model", kSecret);
  check(replaced["ok"] == true && replaced["result"]["has_key"] == true,
        "key replace failed");
  check(fx.keys.value == kSecret, "memory key was not replaced");
  noSecret(replaced.dump(), kSecret);
  auto again = fx.save(false, "https://example.test/v1", "mock-model", nullptr);
  check(again["result"]["enabled"] == false &&
            again["result"]["has_key"] == true,
        "null key cleared the secret");
  check(fx.keys.value == kSecret, "null key deleted the secret");
  auto removed = fx.save(false, "", "", "");
  check(removed["ok"] == true && removed["result"]["has_key"] == false,
        "empty key did not delete");
  check(removed["result"]["base_url"] == p::kDefaultAiBaseUrl,
        "empty url did not restore default");
  check(removed["result"]["model"] == p::kDefaultAiModel,
        "empty model did not restore default");
  check(!fx.keys.value.has_value(), "empty key left a secret");
  auto removedAgain =
      fx.save(false, p::kDefaultAiBaseUrl, p::kDefaultAiModel, "");
  check(removedAgain["ok"] == true &&
            removedAgain["result"]["has_key"] == false,
        "second delete failed");
  scanFile(fx.file, kSecret);
  scanFile(fx.file, "old-key-value");
  noSecret(p::aiTraceLog(), kSecret);
  auto noKey = call(fx.app, "ai_test_connection");
  check(noKey["ok"] == false && noKey["error"]["code"] == "NETWORK_ERROR",
        "missing key succeeded");
  check(noKey["error"]["message"] == "No API key stored for AI",
        "missing key message");
  check(fx.http.calls == 0, "missing key called the network");
  fx.keys.value = kSecret;
  fx.save(true, "https://example.test/v1", "mock-model", nullptr);
  auto tested = call(fx.app, "ai_test_connection");
  check(tested["ok"] == true && tested["result"].is_null(),
        "connection test failed");
  check(fx.http.calls == 1 && fx.http.lastTimeout == p::kAiTimeoutMs,
        "timeout argument");
  check(fx.http.lastUrl == "https://example.test/v1/chat/completions",
        "chat url");
  check(fx.http.bodies.back().find("max_tokens") != std::string::npos,
        "test payload");
  check(fx.http.bodies.back().find(kSecret) == std::string::npos,
        "key entered the JSON body");
  fx.http.mode = "leak";
  auto leaked = call(fx.app, "ai_test_connection");
  check(leaked["ok"] == false, "leaking transport succeeded");
  noSecret(leaked.dump(), kSecret);
  noSecret(p::aiTraceLog(), kSecret);
  scanFile(fx.file, kSecret);
  auto looked = call(fx.app, "lookup_word", {{"word", "meticulous"}});
  check(looked["ok"] == true, "lookup failed after AI error");
}
void testGenerate() {
  Fixture fx("generate");
  insertWord(fx.app, 9100, "zzblank", 9110, "   ", "", 0, "");
  insertWord(fx.app, 9100, "zzblank", 9111, "solid meaning", "中文义", 1,
             "PRIVATE-DICT-EXAMPLE-SHOULD-NOT-LEAK stays here.");
  insertWord(fx.app, 9102, "zzother", 9112, "other meaning", "另一义", 0, "");
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::Statement note(
        db, "INSERT INTO word_notes(word_id,note,updated_at) VALUES(?,?,?)");
    note.bind(1, 9100)
        .bind(2, "PRIVATE-NOTE-SHOULD-NOT-LEAK")
        .bind(3, "2026-10-26T00:00:00.000Z");
    note.step();
    return 0;
  });
  fx.keys.value = kSecret;
  fx.save(false, "https://example.test/v1", "mock-model", nullptr);
  auto disabled = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", 9111}}})}});
  check(disabled["ok"] == true && disabled["result"].empty(),
        "disabled AI did not return empty");
  check(fx.http.calls == 0, "disabled AI called the network");
  fx.save(true, "https://example.test/v1", "mock-model", nullptr);
  fx.keys.value.reset();
  auto noKeyApp = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", 9111}}})}});
  check(noKeyApp["ok"] == false &&
            noKeyApp["error"]["message"] == "No API key stored for AI",
        "generate without key");
  check(fx.http.calls == 0, "generate without key called the network");
  fx.keys.value = kSecret;
  auto badSense = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", 99999}}})}});
  check(badSense["ok"] == true && badSense["result"].empty() &&
            fx.http.calls == 0,
        "foreign sense called the network");
  auto fallback = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", nullptr}}})}});
  check(fallback["ok"] == true && fallback["result"].size() == 1,
        "sense fallback failed");
  check(fallback["result"][0]["sense_id"] == 9111, "fallback sense");
  check(fallback["result"][0]["sentence"] ==
            "People call ______ ______ during the quiet afternoon.",
        "generated cloze");
  check(fx.http.bodies.back().find("solid meaning") != std::string::npos,
        "sense text was not sent");
  check(fx.http.bodies.back().find("中文义") != std::string::npos,
        "chinese sense was not sent");
  check(fx.http.bodies.back().find("PRIVATE-NOTE-SHOULD-NOT-LEAK") ==
            std::string::npos,
        "private note was sent");
  check(fx.http.bodies.back().find("PRIVATE-DICT-EXAMPLE-SHOULD-NOT-LEAK") ==
            std::string::npos,
        "dictionary example was sent");
  check(fx.http.bodies.back().find(kSecret) == std::string::npos,
        "key entered the prompt");
  check(fx.http.lastTimeout == p::kAiTimeoutMs, "generate timeout");
  auto calls = fx.http.calls;
  auto hit = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", nullptr}}})}});
  check(hit["ok"] == true && hit["result"].size() == 1 &&
            fx.http.calls == calls,
        "cache miss");
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::Statement row(
        db, "SELECT cache_key, prompt_text, created_at FROM ai_example_cache");
    check(row.step(), "cache row missing");
    auto parts = splitKey(row.text(0));
    check(parts.size() == 6 && parts[0] == "v2" && parts[1] == "zzblank" &&
              parts[2] == "solid meaning" && parts[3] == "中文义" &&
              parts[4] == "https://example.test/v1" && parts[5] == "mock-model",
          "cache key");
    check(row.text(1) ==
              "People call ______ ______ during the quiet afternoon.",
          "cache stored raw text");
    check(row.text(2) == p::formatUtc(1792540800123LL), "cache timestamp");
    p::Statement example(db, "SELECT text FROM examples WHERE sense_id=9111");
    check(example.step() &&
              example.text(0) ==
                  "PRIVATE-DICT-EXAMPLE-SHOULD-NOT-LEAK stays here.",
          "dictionary example was overwritten");
    return 0;
  });
  fx.save(true, "https://example.test/v1/", "mock-model", nullptr);
  auto slash = call(fx.app, "ai_generate_examples",
                    {{"items", p::Json::array({p::Json{{"word_id", 9100},
                                                       {"sense_id", 9111}}})}});
  check(slash["ok"] == true && fx.http.calls == calls,
        "trailing slash invalidated cache");
  fx.save(true, "https://example.test/v1", "other-model", nullptr);
  auto model = call(fx.app, "ai_generate_examples",
                    {{"items", p::Json::array({p::Json{{"word_id", 9100},
                                                       {"sense_id", 9111}}})}});
  check(model["ok"] == true && fx.http.calls == calls + 1,
        "model change still hit");
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::execute(
        db,
        "UPDATE senses SET english_definition='changed meaning' WHERE id=9111");
    return 0;
  });
  fx.save(true, "https://example.test/v1", "mock-model", nullptr);
  auto changed = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", 9111}}})}});
  check(changed["ok"] == true && fx.http.calls == calls + 2,
        "sense change still hit");
  fx.http.mode = "script";
  fx.http.script = "[{\"word_id\":9102,\"sense_id\":9112,\"sentence\":\"People "
                   "call ZZOTHER zzother during the quiet afternoon.\"}]";
  auto cased = call(fx.app, "ai_generate_examples",
                    {{"items", p::Json::array({p::Json{{"word_id", 9102},
                                                       {"sense_id", 9112}}})}});
  check(cased["ok"] == true &&
            cased["result"][0]["sentence"] ==
                "People call ______ ______ during the quiet afternoon.",
        "mixed case was not blanked");
  fx.http.script = "[{\"word_id\":9102,\"sense_id\":9112,\"sentence\":\"The "
                   "zzotherextra value stays hidden during this test.\"}]";
  fx.save(true, "https://example.test/v1", "substring-model", nullptr);
  auto sub = call(fx.app, "ai_generate_examples",
                  {{"items", p::Json::array({p::Json{{"word_id", 9102},
                                                     {"sense_id", 9112}}})}});
  check(sub["ok"] == true && sub["result"].empty(),
        "substring target was cached");
  auto before = fx.http.calls;
  fx.http.mode = "bad-json";
  fx.save(true, "https://example.test/v1", "bad-json-model", nullptr);
  auto bad = call(fx.app, "ai_generate_examples",
                  {{"items", p::Json::array({p::Json{{"word_id", 9102},
                                                     {"sense_id", 9112}}})}});
  check(bad["ok"] == false && bad["error"]["code"] == "NETWORK_ERROR" &&
            fx.http.calls == before + 1,
        "invalid JSON was retried or accepted");
  fx.http.mode = "missing";
  auto missing = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9102}, {"sense_id", 9112}}})}});
  check(missing["ok"] == false &&
            missing["error"]["message"].get<std::string>().find("missing") !=
                std::string::npos,
        "missing choices succeeded");
  fx.http.mode = "plain";
  fx.http.script = "I cannot help with that.";
  fx.http.mode = "script";
  fx.http.script = "I cannot help with that.";
  auto prose = call(fx.app, "ai_generate_examples",
                    {{"items", p::Json::array({p::Json{{"word_id", 9102},
                                                       {"sense_id", 9112}}})}});
  check(prose["ok"] == true && prose["result"].empty(),
        "unusable content became an error");
  fx.http.mode = "timeout";
  auto beforeTimeout = fx.http.calls;
  auto timed = call(fx.app, "ai_generate_examples",
                    {{"items", p::Json::array({p::Json{{"word_id", 9102},
                                                       {"sense_id", 9112}}})}});
  check(timed["ok"] == false &&
            timed["error"]["message"] == "AI request timed out" &&
            fx.http.calls == beforeTimeout + 1,
        "timeout status");
  fx.http.mode = "fail";
  fx.save(true, "https://example.test/v1", "mock-model", nullptr);
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::Statement drop(db, "DELETE FROM ai_example_cache WHERE word_id=?");
    drop.bind(1, 9102);
    drop.step();
    return 0;
  });
  auto failed = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", 9111}},
                        p::Json{{"word_id", 9102}, {"sense_id", 9112}}})}});
  check(failed["ok"] == true && failed["result"].size() == 1 &&
            failed["result"][0]["word_id"] == 9100,
        "cached example was dropped after a network failure");
  fx.keys.failGet = true;
  fx.http.mode = "mirror";
  auto beforeRead = fx.http.calls;
  auto stored = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9100}, {"sense_id", 9111}},
                        p::Json{{"word_id", 9102}, {"sense_id", 9112}}})}});
  check(stored["ok"] == true && stored["result"].size() == 1 &&
            fx.http.calls == beforeRead,
        "credential read failure dropped cache");
  fx.save(false, "https://example.test/v1", "mock-model", nullptr);
  auto off = call(fx.app, "ai_generate_examples",
                  {{"items", p::Json::array({p::Json{{"word_id", 9100},
                                                     {"sense_id", 9111}}})}});
  check(off["ok"] == true && off["result"].empty(),
        "disabled AI returned cached text");
  check(call(fx.app, "lookup_word", {{"word", "meticulous"}})["ok"] == true,
        "lookup failed after generate errors");
  noSecret(failed.dump() + timed.dump() + p::aiTraceLog(), kSecret);
  scanFile(fx.file, kSecret);
}
void testLockAndConcurrency() {
  Fixture fx("concurrent");
  insertWord(fx.app, 9201, "zzone", 9211, "first meaning", "一", 0, "");
  insertWord(fx.app, 9202, "zztwo", 9212, "second meaning", "二", 0, "");
  fx.keys.value = kSecret;
  fx.save(true, "https://example.test/v1", "mock-model", nullptr);
  fx.http.watchLock = true;
  auto locked = call(
      fx.app, "ai_generate_examples",
      {{"items",
        p::Json::array({p::Json{{"word_id", 9201}, {"sense_id", 9211}}})}});
  check(locked["ok"] == true && fx.http.lockFree,
        "database lock held during network");
  fx.http.watchLock = false;
  fx.http.joinProbe();
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DELETE FROM ai_example_cache");
    return 0;
  });
  fx.http.releaseAt = 2;
  p::Json left;
  p::Json right;
  std::thread a([&] {
    left = call(fx.app, "ai_generate_examples",
                {{"items", p::Json::array({p::Json{{"word_id", 9201},
                                                   {"sense_id", 9211}}})}});
  });
  std::thread b([&] {
    right = call(fx.app, "ai_generate_examples",
                 {{"items", p::Json::array({p::Json{{"word_id", 9202},
                                                    {"sense_id", 9212}}})}});
  });
  a.join();
  b.join();
  check(left["ok"] == true && right["ok"] == true,
        "concurrent generate failed");
  check(fx.http.maxInflight >= 2, "network calls were serialized");
  fx.http.releaseAt = 1;
}
void testEnrich() {
  Fixture fx("enrich");
  insertWord(fx.app, 9300, "zzkeep", 9310, "kept meaning", "保留", 0,
             "PRIVATE-DICT-EXAMPLE-SHOULD-NOT-LEAK stays here.");
  insertWord(fx.app, 9301, "zzbad", 9311, "bad meaning", "坏", 0, "");
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::Statement row(
        db, "INSERT INTO word_enrichments(word_id,mnemonic,updated_at) "
            "VALUES(?,?,?)");
    row.bind(1, 9300).bind(2, "keep-me").bind(3, "2026-10-26T00:00:00.000Z");
    row.step();
    p::Statement note(
        db, "INSERT INTO word_notes(word_id,note,updated_at) VALUES(?,?,?)");
    note.bind(1, 9300)
        .bind(2, "PRIVATE-NOTE-SHOULD-NOT-LEAK")
        .bind(3, "2026-10-26T00:00:00.000Z");
    note.step();
    return 0;
  });
  auto off = call(fx.app, "ai_enrich_words",
                  {{"wordIds", p::Json::array({9300})}, {"examStyle", true}});
  check(off["ok"] == true && off["result"] == 0 && fx.http.calls == 0,
        "disabled enrich called the network");
  fx.keys.value = kSecret;
  fx.save(true, "https://example.test/v1", "mock-model", nullptr);
  fx.keys.value.reset();
  auto missing =
      call(fx.app, "ai_enrich_words",
           {{"wordIds", p::Json::array({9300})}, {"examStyle", true}});
  check(missing["ok"] == true && missing["result"] == 0 && fx.http.calls == 0,
        "enrich without key failed the local flow");
  fx.keys.value = kSecret;
  auto filled = call(
      fx.app, "ai_enrich_words",
      {{"wordIds", p::Json::array({9300, 9300, -1})}, {"examStyle", true}});
  check(filled["ok"] == true && filled["result"] == 1, "enrich count");
  check(fx.http.calls == 1, "duplicate ids were sent twice");
  check(fx.http.bodies.back().find("CET-4") != std::string::npos,
        "exam prompt");
  check(fx.http.bodies.back().find("PRIVATE-NOTE-SHOULD-NOT-LEAK") ==
            std::string::npos,
        "enrich sent a private note");
  check(fx.http.lastTimeout == p::kAiTimeoutMs, "enrich timeout");
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::Statement row(
        db, "SELECT mnemonic, roots_json, exam_example_text, "
            "exam_example_translation, "
            "standard_example_text FROM word_enrichments WHERE word_id=9300");
    check(row.step(), "enrichment row missing");
    check(row.text(0) == "keep-me", "existing mnemonic was overwritten");
    check(row.text(1).find("zzkeep") != std::string::npos,
          "roots were not stored");
    check(row.text(2).find("zzkeep") != std::string::npos &&
              row.text(2).find("______") == std::string::npos,
          "enrichment example was blanked or lost the target");
    check(!row.text(3).empty(), "translation missing");
    check(row.isNull(4), "standard example was invented");
    p::Statement example(db, "SELECT text FROM examples WHERE sense_id=9310");
    check(example.step() &&
              example.text(0).find("PRIVATE-DICT-EXAMPLE-SHOULD-NOT-LEAK") == 0,
          "dictionary example changed");
    return 0;
  });
  auto again =
      call(fx.app, "ai_enrich_words",
           {{"wordIds", p::Json::array({9300})}, {"examStyle", false}});
  check(again["result"] == 0 && fx.http.calls == 1,
        "complete enrichment called the network again");
  fx.http.mode = "script";
  fx.http.script = "[{\"word_id\":99999,\"mnemonic\":\"x\",\"roots\":[{"
                   "\"part\":\"zzbad\",\"meaning\":\"义\"}],"
                   "\"example\":\"The zzbad stays visible in this "
                   "sentence.\",\"translation\":\"译\"},"
                   "{\"word_id\":9301,\"mnemonic\":\"  "
                   "\",\"roots\":[{\"part\":\"zzbad\",\"meaning\":\"义\"}],"
                   "\"example\":\"The zzbad stays visible in this "
                   "sentence.\",\"translation\":\"译\"},"
                   "{\"word_id\":9301,\"mnemonic\":\"助记\",\"roots\":[{"
                   "\"part\":\"\",\"meaning\":\"\"}],"
                   "\"example\":\"The zzbad stays visible in this "
                   "sentence.\",\"translation\":\"译\"},"
                   "{\"word_id\":9301,\"mnemonic\":\"助记\",\"roots\":[{"
                   "\"part\":\"zzbad\",\"meaning\":\"义\"}],"
                   "\"example\":\"The zzbadextra stays visible in this "
                   "sentence.\",\"translation\":\"译\"},"
                   "{\"word_id\":9301,\"mnemonic\":\"可用助记\",\"roots\":[{"
                   "\"part\":\"zzbad\",\"meaning\":\"义项\"}],"
                   "\"example\":\"The zzbad stays visible in this "
                   "sentence.\",\"translation\":\"可用译文\"},"
                   "{\"word_id\":9301,\"mnemonic\":\"重复\",\"roots\":[{"
                   "\"part\":\"zzbad\",\"meaning\":\"义项\"}],"
                   "\"example\":\"The zzbad stays visible in this "
                   "sentence.\",\"translation\":\"重复译文\"}]";
  auto invalid =
      call(fx.app, "ai_enrich_words",
           {{"wordIds", p::Json::array({9301})}, {"examStyle", false}});
  check(invalid["ok"] == true && invalid["result"] == 1,
        "invalid enrichment was stored");
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::Statement row(
        db, "SELECT mnemonic FROM word_enrichments WHERE word_id=9301");
    check(row.step() && row.text(0) == "可用助记", "wrong enrichment row won");
    p::execute(db, "DELETE FROM word_enrichments");
    return 0;
  });
  fx.http.mode = "mirror";
  fx.http.bodies.clear();
  auto everyday =
      call(fx.app, "ai_enrich_words",
           {{"wordIds", p::Json::array({9301})}, {"examStyle", false}});
  check(everyday["result"] == 1 &&
            fx.http.bodies.back().find("everyday English") != std::string::npos,
        "everyday prompt");
  check(fx.http.bodies.back().find("CET-4") == std::string::npos,
        "everyday prompt used exam style");
  for (int i = 0; i < 11; ++i)
    insertWord(fx.app, 9400 + i, "zzbatch" + std::to_string(i), 9410 + i,
               "batch meaning", "批", 0, "");
  fx.http.bodies.clear();
  fx.http.calls = 0;
  p::Json ids = p::Json::array();
  for (int i = 0; i < 11; ++i)
    ids.push_back(9400 + i);
  auto batch =
      call(fx.app, "ai_enrich_words", {{"wordIds", ids}, {"examStyle", true}});
  check(batch["ok"] == true && batch["result"] == 11 && fx.http.calls == 2,
        "batching");
  check(fx.http.bodies.size() == 2, "batch request count");
  auto countIds = [](const std::string &body) {
    int count = 0;
    for (std::size_t at = 0;
         (at = body.find("word_id:", at)) != std::string::npos; at += 8)
      ++count;
    return count;
  };
  check(countIds(fx.http.bodies[0]) == 10 && countIds(fx.http.bodies[1]) == 1,
        "batch sizes");
  fx.http.mode = "timeout";
  fx.app.database().withConnection([&](sqlite3 *db) {
    p::execute(db, "DELETE FROM word_enrichments WHERE word_id=9301");
    return 0;
  });
  auto timed =
      call(fx.app, "ai_enrich_words",
           {{"wordIds", p::Json::array({9301})}, {"examStyle", false}});
  check(timed["ok"] == false && timed["error"]["code"] == "NETWORK_ERROR",
        "enrich timeout status");
  check(call(fx.app, "lookup_word", {{"word", "meticulous"}})["ok"] == true,
        "lookup after enrich failure");
  noSecret(filled.dump() + timed.dump() + p::aiTraceLog(), kSecret);
  scanFile(fx.file, kSecret);
}
struct LocalServer {
  SOCKET listen = INVALID_SOCKET;
  std::atomic<int> accepts{0};
  std::atomic<bool> stop{false};
  std::thread thread;
  int port = 0;
  std::string response;
  std::string request;
  bool hold = false;
  LocalServer(std::string payload, bool hold)
      : response(std::move(payload)), hold(hold) {
    listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(listen != INVALID_SOCKET, "socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0,
          "bind");
    check(::listen(listen, 1) == 0, "listen");
    int length = sizeof(addr);
    check(getsockname(listen, reinterpret_cast<sockaddr *>(&addr), &length) ==
              0,
          "port");
    port = ntohs(addr.sin_port);
    thread = std::thread([this] { serve(); });
  }
  ~LocalServer() {
    stop = true;
    if (listen != INVALID_SOCKET)
      closesocket(listen);
    if (thread.joinable())
      thread.join();
  }
  void serve() {
    SOCKET client = accept(listen, nullptr, nullptr);
    if (client == INVALID_SOCKET)
      return;
    ++accepts;
    std::string data;
    char buffer[1024];
    while (data.find("\r\n\r\n") == std::string::npos) {
      auto n = recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        break;
      data.append(buffer, buffer + n);
    }
    request = std::move(data);
    if (hold) {
      while (!stop)
        Sleep(20);
    } else {
      send(client, response.data(), static_cast<int>(response.size()), 0);
    }
    closesocket(client);
  }
};
void testRealHttp() {
  WSADATA data{};
  check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "winsock");
  struct Cleanup {
    ~Cleanup() { WSACleanup(); }
  } cleanup;
  const std::string secret = "sk-parity-HTTP-77ab";
  auto status = "HTTP/1.1 401 Unauthorized\r\nContent-Type: "
                "text/plain\r\nContent-Length: " +
                std::to_string(secret.size()) +
                "\r\nConnection: close\r\n\r\n" + secret;
  LocalServer denied(status, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  p::WinHttpClient client(false);
  bool threw = false;
  try {
    client.postJson("http://127.0.0.1:" + std::to_string(denied.port) +
                        "/v1/chat/completions",
                    secret, "{\"model\":\"m\",\"messages\":[]}", 2000);
  } catch (const p::Error &error) {
    threw = true;
    check(error.code == "NETWORK_ERROR", "http status code");
    noSecret(error.what(), secret);
    check(std::string(error.what()).find("401") != std::string::npos,
          "status was hidden");
  }
  check(threw, "401 was treated as success");
  check(denied.accepts.load() == 1, "status request count");
  check(denied.request.find("Authorization: Bearer " + secret) !=
            std::string::npos,
        "auth header");
  check(denied.request.find("/v1/chat/completions") != std::string::npos,
        "chat path");
  LocalServer hanging("", true);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto started = std::chrono::steady_clock::now();
  threw = false;
  try {
    client.postJson("http://127.0.0.1:" + std::to_string(hanging.port) +
                        "/v1/chat/completions",
                    "timeout-token", "{\"model\":\"m\",\"messages\":[]}", 2500);
  } catch (const p::Error &error) {
    threw = true;
    check(std::string(error.what()).find("timed out") != std::string::npos,
          "timeout text");
    noSecret(error.what(), "timeout-token");
  }
  auto elapsed = std::chrono::steady_clock::now() - started;
  auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  check(threw, "hang was not timed out");
  check(elapsedMs > 1000 && elapsedMs < 7000 && hanging.accepts.load() == 1,
        "timeout waited " + std::to_string(elapsedMs) + " ms across " +
            std::to_string(hanging.accepts.load()) + " connections");

  LocalServer cancelServer("", true);
  p::Cancellation cancellation;
  std::thread cancelThread([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancellation.request();
  });
  p::setCurrentCancellation(&cancellation);
  const auto cancelStart = std::chrono::steady_clock::now();
  bool cancelled = false;
  try {
    client.postJson("http://127.0.0.1:" + std::to_string(cancelServer.port) +
                        "/v1/chat/completions",
                    "cancel-token", "{}", 15000);
  } catch (const p::Error &error) {
    cancelled = error.code == "CANCELLED";
  }
  p::setCurrentCancellation(nullptr);
  cancelThread.join();
  check(cancelled, "in-flight HTTP request ignored cancellation");
  check(std::chrono::steady_clock::now() - cancelStart <
            std::chrono::seconds(2),
        "HTTP cancellation waited for the network timeout");
}
void testRealCredentials() {
  auto target = L"LexicaReproduce/parity-ai-test/" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64());
  check(std::wstring(target).find(L"com.lexica.app") == std::wstring::npos,
        "target uses upstream service");
  check(p::productionKeyStore().target() ==
            L"LexicaReproduce.parity-v1/ai_api_key",
        "production target");
  check(p::productionKeyStore().target() != target,
        "test reused the production target");
  struct Guard {
    p::WindowsCredentialStore store;
    explicit Guard(std::wstring name) : store(std::move(name)) {}
    ~Guard() {
      try {
        store.remove();
      } catch (...) {
      }
    }
  } guard(target);
  guard.store.remove();
  check(!guard.store.get().has_value(), "empty target already had a secret");
  guard.store.set(kWinSecret);
  check(guard.store.get() == kWinSecret, "credential roundtrip");
  guard.store.set("sk-parity-WINCRED-replaced");
  check(guard.store.get() == "sk-parity-WINCRED-replaced",
        "credential replace");
  guard.store.remove();
  check(!guard.store.get().has_value(), "credential delete");
  guard.store.remove();
  PCREDENTIALW cred = nullptr;
  check(!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred),
        "credential remained after delete");
  check(GetLastError() == ERROR_NOT_FOUND,
        "credential delete was not a clean miss");
  Fixture fx("wincred");
  p::setAiRuntime(&guard.store, &fx.http);
  auto saved =
      fx.save(true, "https://example.test/v1", "mock-model", kWinSecret);
  check(saved["ok"] == true && saved["result"]["has_key"] == true,
        "real key save");
  check(saved["result"].contains("key") == false, "response exposed key field");
  noSecret(saved.dump(), kWinSecret);
  check(guard.store.get() == kWinSecret,
        "command did not reach the credential manager");
  scanFile(fx.file, kWinSecret);
  noSecret(p::aiTraceLog(), kWinSecret);
  auto cleared = fx.save(true, "https://example.test/v1", "mock-model", "");
  check(cleared["ok"] == true && cleared["result"]["has_key"] == false,
        "real key delete");
  check(!guard.store.get().has_value(), "command delete left the credential");
  cred = nullptr;
  check(!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred),
        "command left a credential");
}
int main() {
  try {
    testText();
    testConfig();
    testGenerate();
    testLockAndConcurrency();
    testEnrich();
    testRealHttp();
    testRealCredentials();
    std::cout << "parity_ai_test ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
