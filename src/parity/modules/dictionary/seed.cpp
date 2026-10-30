#include "parity/modules/dictionary/api.h"
#include "parity_resources.h"
#include <algorithm>
#include <unordered_map>
namespace lexica::parity {
namespace {
std::string joinTags(const Json &tags) {
  std::string out;
  if (!tags.is_array())
    return out;
  for (const auto &tag : tags) {
    if (!tag.is_string())
      continue;
    const auto text = tag.get<std::string>();
    if (text.empty())
      continue;
    if (!out.empty())
      out.push_back(' ');
    out += text;
  }
  return out;
}
bool tagMeansOxford(const Json &tags) {
  if (!tags.is_array())
    return false;
  for (const auto &tag : tags)
    if (tag.is_string()) {
      auto text = tag.get<std::string>();
      for (char &ch : text)
        if (ch >= 'A' && ch <= 'Z')
          ch = static_cast<char>(ch - 'A' + 'a');
      if (text == "oxford")
        return true;
    }
  return false;
}
const std::vector<SeedRecord> &seedRecords() {
  static const auto records = [] {
    std::vector<SeedRecord> rows;
    const auto seed = Json::parse(kSeedJson);
    for (const auto &word : seed.at("words")) {
      SeedRecord row;
      row.word = normalizeWord(word.at("word").get<std::string>());
      if (row.word.empty())
        continue;
      if (word.contains("phonetic") && word["phonetic"].is_string()) {
        const auto phonetic = word["phonetic"].get<std::string>();
        if (!phonetic.empty())
          row.phonetic = phonetic;
      }
      if (word.contains("frequency_rank") && word["frequency_rank"].is_number_integer())
        row.frequency = word["frequency_rank"].get<std::int64_t>();
      row.synonyms = word.value("synonyms", Json::array()).dump();
      row.antonyms = word.value("antonyms", Json::array()).dump();
      row.wordFamily = word.value("word_family", Json::array()).dump();
      row.tags = joinTags(word.value("tags", Json::array()));
      if (word.contains("collins") && word["collins"].is_number_integer()) {
        const auto collins = word["collins"].get<std::int64_t>();
        if (collins > 0)
          row.collins = collins;
      }
      if (word.contains("oxford") && word["oxford"].is_boolean() &&
          word["oxford"].get<bool>())
        row.oxford = true;
      if (!row.oxford)
        row.oxford = tagMeansOxford(word.value("tags", Json::array()));
      if (word.contains("mnemonic") && word["mnemonic"].is_string()) {
        const auto mnemonic = trimCopy(word["mnemonic"].get<std::string>());
        if (!mnemonic.empty())
          row.mnemonic = mnemonic;
      }
      const auto roots = word.value("roots", Json::array());
      for (const auto &root : roots)
        if (root.is_object() && root.contains("part") && root.contains("meaning"))
          row.roots.push_back(
              {root.at("part").get<std::string>(), root.at("meaning").get<std::string>()});
      const auto forms = word.value("forms", Json::object());
      for (const auto &form : forms.items()) {
        const auto normalized = normalizeWord(form.value().get<std::string>());
        const char *kind = mapFormKind(form.key());
        if (!normalized.empty() && normalized != row.word && kind) {
          // Storage has one kind per (form, word). Preserve the previous final
          // kind without rewriting the same row for past/past-participle aliases.
          const auto alias = std::find_if(row.forms.begin(), row.forms.end(),
              [&](const auto &item) { return item.first == normalized; });
          if (alias == row.forms.end())
            row.forms.emplace_back(normalized, kind);
          else
            alias->second = kind;
        }
      }
      rows.push_back(std::move(row));
    }
    return rows;
  }();
  return records;
}
const std::unordered_map<std::string, const SeedRecord *> &seedIndex() {
  static const auto index = [] {
    std::unordered_map<std::string, const SeedRecord *> map;
    for (const auto &row : seedRecords())
      map.emplace(row.word, &row);
    return map;
  }();
  return index;
}
void bindMaybe(Statement &statement, int index,
               const std::optional<std::string> &value) {
  if (value)
    statement.bind(index, *value);
  else
    statement.null(index);
}
void bindMaybe(Statement &statement, int index,
               const std::optional<std::int64_t> &value) {
  if (value)
    statement.bind(index, *value);
  else
    statement.null(index);
}
} // namespace
const SeedRecord *findSeedRecord(const std::string &normalizedWord) {
  const auto found = seedIndex().find(normalizedWord);
  return found == seedIndex().end() ? nullptr : found->second;
}
void backfillSeedMetadata(sqlite3 *db, const std::string &now) {
  if (!sqlite3_get_autocommit(db))
    return;
  {
    Transaction tx(db);
    for (const auto &row : seedRecords()) {
      Statement existing(db, "SELECT id, source FROM words WHERE word=?");
      existing.bind(1, row.word);
      if (!existing.step() || existing.text(1) != "seed")
        continue;
      const auto id = existing.integer(0);
      Statement update(
          db,
          "UPDATE words SET phonetic=CASE WHEN phonetic IS NULL OR phonetic='' "
          "THEN COALESCE(?1, phonetic) ELSE phonetic END, frequency_rank=COALESCE(?2, "
          "frequency_rank), synonyms=CASE WHEN ?3<>'[]' THEN ?4 ELSE synonyms END, "
          "antonyms=CASE WHEN ?5<>'[]' THEN ?6 ELSE antonyms END, "
          "word_family=CASE WHEN ?7<>'[]' THEN ?8 ELSE word_family END, "
          "tags=CASE WHEN ?9<>'' THEN ?10 ELSE tags END, collins=COALESCE(?11, collins), "
          "oxford=CASE WHEN ?12>0 THEN ?13 ELSE oxford END WHERE id=?14 AND ("
          "((phonetic IS NULL OR phonetic='') AND phonetic IS NOT COALESCE(?1,phonetic)) OR "
          "(?2 IS NOT NULL AND frequency_rank IS NOT ?2) OR "
          "(?3<>'[]' AND synonyms IS NOT ?4) OR (?5<>'[]' AND antonyms IS NOT ?6) OR "
          "(?7<>'[]' AND word_family IS NOT ?8) OR (?9<>'' AND tags IS NOT ?10) OR "
          "(?11 IS NOT NULL AND collins IS NOT ?11) OR (?12>0 AND oxford IS NOT ?13))");
      bindMaybe(update, 1, row.phonetic);
      bindMaybe(update, 2, row.frequency);
      update.bind(3, row.synonyms).bind(4, row.synonyms);
      update.bind(5, row.antonyms).bind(6, row.antonyms);
      update.bind(7, row.wordFamily).bind(8, row.wordFamily);
      update.bind(9, row.tags).bind(10, row.tags);
      bindMaybe(update, 11, row.collins);
      update.bind(12, row.oxford ? std::int64_t{1} : std::int64_t{0});
      update.bind(13, row.oxford ? std::int64_t{1} : std::int64_t{0});
      update.bind(14, id).step();
      for (const auto &form : row.forms) {
        Statement kind(db, "UPDATE word_forms SET kind=?1 WHERE word_id=?2 AND form=?3 AND kind IS NOT ?1");
        kind.bind(1, form.second).bind(2, id).bind(3, form.first).step();
      }
      if (row.mnemonic || !row.roots.empty()) {
        Json roots = Json::array();
        for (const auto &root : row.roots)
          roots.push_back({{"part", root.part}, {"meaning", root.meaning}});
        Statement enrichment(
            db,
            "INSERT INTO word_enrichments(word_id,mnemonic,roots_json,"
            "exam_example_text,exam_example_translation,standard_example_text,"
            "standard_example_translation,updated_at) VALUES(?,?,?,NULL,NULL,NULL,NULL,?) "
            "ON CONFLICT(word_id) DO UPDATE SET "
            "mnemonic=COALESCE(word_enrichments.mnemonic, excluded.mnemonic), "
            "roots_json=CASE WHEN word_enrichments.roots_json IN ('', '[]') "
            "THEN excluded.roots_json ELSE word_enrichments.roots_json END "
            "WHERE (word_enrichments.mnemonic IS NULL AND excluded.mnemonic IS NOT NULL) "
            "OR (word_enrichments.roots_json IN ('', '[]') AND excluded.roots_json<>'[]')");
        enrichment.bind(1, id);
        bindMaybe(enrichment, 2, row.mnemonic);
        enrichment.bind(3, roots.dump()).bind(4, now).step();
      }
    }
    tx.commit();
  }
}
} // namespace lexica::parity
