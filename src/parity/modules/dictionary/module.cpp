#include "parity/modules/modules.h"
#include <algorithm>
#include <cctype>
namespace lexica::parity {
namespace {
// Phase 0 seed query only. Agent B owns full upstream Unicode normalization.
std::string seedNormalize(std::string word) {
  const auto begin = word.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  word = word.substr(begin, word.find_last_not_of(" \t\r\n") - begin + 1);
  std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return word;
}
std::optional<std::string> maybeText(const Statement &s, int col) {
  if (s.isNull(col))
    return std::nullopt;
  return s.text(col);
}
class SeedDictionary final : public DictionaryPort {
public:
  std::optional<WordEntry> byId(sqlite3 *db, std::int64_t id) override {
    Statement q(db, "SELECT "
                    "id,word,display,phonetic,frequency_rank,synonyms,antonyms,"
                    "word_family,tags,collins,oxford FROM words WHERE id=?");
    q.bind(1, id);
    if (!q.step())
      return std::nullopt;
    WordEntry e;
    e.id = q.integer(0);
    e.word = q.text(1);
    e.display = q.text(2);
    e.phonetic = maybeText(q, 3);
    if (!q.isNull(4))
      e.frequency_rank = q.integer(4);
    e.synonyms = Json::parse(q.text(5)).get<std::vector<std::string>>();
    e.antonyms = Json::parse(q.text(6)).get<std::vector<std::string>>();
    e.word_family = Json::parse(q.text(7)).get<std::vector<std::string>>();
    if (!q.isNull(9))
      e.collins = q.integer(9);
    e.oxford = q.integer(10) != 0;
    Statement senses(
        db, "SELECT id,pos,english_definition,chinese_definition,level FROM "
            "senses WHERE word_id=? ORDER BY order_idx,id");
    senses.bind(1, id);
    while (senses.step()) {
      Sense s;
      s.id = senses.integer(0);
      s.pos = maybeText(senses, 1);
      s.english_definition = senses.text(2);
      s.chinese_definition = maybeText(senses, 3);
      s.level = maybeText(senses, 4);
      Statement examples(db, "SELECT id,text,translation FROM examples WHERE "
                             "sense_id=? ORDER BY order_idx,id");
      examples.bind(1, s.id);
      while (examples.step())
        s.examples.push_back(Example{examples.integer(0), examples.text(1),
                                     maybeText(examples, 2)});
      e.senses.push_back(std::move(s));
    }
    Statement collocations(db, "SELECT text,gloss FROM collocations WHERE "
                               "word_id=? ORDER BY order_idx,id");
    collocations.bind(1, id);
    while (collocations.step())
      e.collocations.push_back(
          {collocations.text(0), maybeText(collocations, 1)});
    Statement forms(
        db,
        "SELECT kind,form FROM word_forms WHERE word_id=? ORDER BY kind,form");
    forms.bind(1, id);
    while (forms.step())
      e.forms.push_back({forms.text(0), forms.text(1)});
    return e;
  }
  std::optional<WordEntry> find(sqlite3 *db,
                                const std::string &input) override {
    const auto word = seedNormalize(input);
    if (word.empty())
      return std::nullopt;
    Statement exact(db, "SELECT id FROM words WHERE word=?");
    exact.bind(1, word);
    if (exact.step())
      return byId(db, exact.integer(0));
    Statement form(
        db,
        "SELECT word_id FROM word_forms WHERE form=? ORDER BY word_id LIMIT 1");
    form.bind(1, word);
    if (form.step())
      return byId(db, form.integer(0));
    return std::nullopt;
  }
  std::vector<Suggestion> suggest(sqlite3 *db, const std::string &input,
                                  int limit) override {
    const auto word = seedNormalize(input);
    std::vector<Suggestion> result;
    if (word.empty() || limit <= 0)
      return result;
    Statement q(db, "SELECT word,display,phonetic,frequency_rank FROM words "
                    "WHERE word>=? AND word<? ORDER BY word<>?,frequency_rank "
                    "IS NULL,frequency_rank,word LIMIT ?");
    q.bind(1, word)
        .bind(2, word + "\xF4\x8F\xBF\xBF")
        .bind(3, word)
        .bind(4, limit);
    while (q.step()) {
      Suggestion s;
      s.word = q.text(0);
      s.display = q.text(1);
      s.phonetic = maybeText(q, 2);
      if (!q.isNull(3))
        s.frequency_rank = q.integer(3);
      result.push_back(std::move(s));
    }
    return result;
  }
};
} // namespace
std::unique_ptr<DictionaryPort> makeDictionary() {
  return std::make_unique<SeedDictionary>();
}
void registerDictionary(Router &router) {
  router.bind("dictionary", "app_info", [](const Json &, Context &c) -> Json {
    return c.db.withConnection([&](sqlite3 *db) -> Json {
      Statement version(db, "PRAGMA user_version");
      version.step();
      Statement count(db, "SELECT COUNT(*) FROM words");
      count.step();
      Statement fts(db, "SELECT rowid FROM words_fts LIMIT 1");
      fts.step();
      Statement ecdict(db, "SELECT COUNT(*) FROM words WHERE source='ecdict'");
      ecdict.step();
      return AppInfo{version.integer(0), count.integer(0), true,
                     ecdict.integer(0) >= 1000000};
    });
  });
  router.bind(
      "dictionary", "search_suggest",
      [](const Json &a, Context &c) -> Json {
        const auto args = a.get<SearchSuggestArgs>();
        return c.db.withConnection([&](sqlite3 *db) -> Json {
          return c.dictionary.suggest(db, args.query,
                                      static_cast<int>(args.limit.value_or(8)));
        });
      },
      "seed_only");
}
} // namespace lexica::parity
