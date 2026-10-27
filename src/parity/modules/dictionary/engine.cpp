#include "parity/modules/dictionary/api.h"
#include "parity/modules/dictionary/roots.h"
#include <algorithm>
#include <limits>
#include <unordered_set>
namespace lexica::parity {
namespace {
std::optional<std::string> columnText(const Statement &statement, int column) {
  if (statement.isNull(column))
    return std::nullopt;
  return statement.text(column);
}
std::vector<std::string> parseStringList(const std::string &raw) {
  try {
    return Json::parse(raw.empty() ? "[]" : raw).get<std::vector<std::string>>();
  } catch (const Json::exception &) {
    throw Error("INTERNAL_ERROR", "Dictionary list field is not a JSON array");
  }
}
std::vector<std::string> parseTags(std::string raw) {
  raw = trimCopy(raw);
  if (raw.empty())
    return {};
  if (raw.front() == '[') {
    try {
      std::vector<std::string> tags;
      for (const auto &item : Json::parse(raw)) {
        if (!item.is_string())
          continue;
        auto text = trimCopy(item.get<std::string>());
        if (!text.empty())
          tags.push_back(std::move(text));
      }
      return tags;
    } catch (const Json::exception &) {
    }
  }
  std::vector<std::string> tags;
  std::string current;
  auto flush = [&] {
    auto text = trimCopy(current);
    if (!text.empty())
      tags.push_back(std::move(text));
    current.clear();
  };
  for (char ch : raw) {
    if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
      flush();
    else
      current.push_back(ch);
  }
  flush();
  return tags;
}
bool equalsIgnoreCase(std::string left, const std::string &right) {
  for (char &ch : left)
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
  return left == right;
}
std::optional<std::string> normalizeCefr(const std::optional<std::string> &raw) {
  if (!raw)
    return std::nullopt;
  auto value = trimCopy(*raw);
  for (char &ch : value)
    if (ch >= 'a' && ch <= 'z')
      ch = static_cast<char>(ch - 'a' + 'A');
  if (value == "A1" || value == "A2" || value == "B1" || value == "B2" ||
      value == "C1" || value == "C2")
    return value;
  return std::nullopt;
}
std::string inferFormLabel(const std::string &headword, const std::string &form) {
  if (endsWith(form, "ing") && !endsWith(headword, "ing"))
    return "Present participle";
  if (endsWith(form, "iest") ||
      (endsWith(form, "est") && !endsWith(headword, "est")))
    return "Superlative";
  if (endsWith(form, "ier") || (endsWith(form, "er") && !endsWith(headword, "er")))
    return "Comparative";
  if (endsWith(form, "ed") && !endsWith(headword, "ed"))
    return "Past / Past participle";
  if (endsWith(form, "ies") || endsWith(form, "s"))
    return "Plural / 3rd singular";
  return "Inflection";
}
std::string formLabel(const std::string &raw, const std::string &headword,
                      const std::string &form) {
  if (raw.empty() || raw == "form")
    return inferFormLabel(headword, form);
  if (const char *mapped = mapFormKind(raw))
    return mapped;
  return raw;
}
std::optional<std::pair<std::string, std::string>>
posPrefix(const std::string &text) {
  const auto trimmed = trimCopy(text);
  const std::pair<const char *, const char *> items[] = {
      {"noun ", "noun"}, {"verb ", "verb"}, {"n.", "n."},   {"vt.", "vt."},
      {"vi.", "vi."},    {"v.", "v."},      {"adj.", "adj."}, {"adv.", "adv."},
      {"prep.", "prep."}, {"conj.", "conj."}, {"pron.", "pron."},
      {"a.", "adj."},    {"s.", "adj."},    {"r.", "adv."}};
  for (const auto &item : items) {
    const std::string prefix = item.first;
    if (trimmed.rfind(prefix, 0) != 0)
      continue;
    const auto rest = trimmed.substr(prefix.size());
    if (rest.empty() || rest.find_first_not_of(" \t\r\n") != 0)
      return std::make_pair(std::string(item.second), trimCopy(rest));
  }
  return std::nullopt;
}
std::pair<std::optional<std::string>, std::string>
inferPosAndStrip(const std::string &english,
                 const std::optional<std::string> &chinese) {
  if (english.find('\n') == std::string::npos)
    if (const auto hit = posPrefix(english))
      return {hit->first, hit->second};
  if (chinese)
    if (const auto hit = posPrefix(*chinese))
      return {hit->first, std::string{}};
  return {std::nullopt, {}};
}
std::optional<std::int64_t> wordIdWhere(sqlite3 *db, const std::string &sql,
                                        const std::string &key) {
  Statement query(db, sql);
  query.bind(1, key);
  if (!query.step())
    return std::nullopt;
  return query.integer(0);
}
Suggestion readSuggestion(const Statement &query) {
  Suggestion suggestion;
  suggestion.word = query.text(0);
  suggestion.display = query.text(1);
  suggestion.phonetic = columnText(query, 2);
  if (!query.isNull(3))
    suggestion.frequency_rank = query.integer(3);
  return suggestion;
}
std::optional<std::pair<std::optional<std::string>, std::string>>
lookupGloss(sqlite3 *db, const std::string &target) {
  const auto word = normalizeWord(target);
  if (word.empty())
    return std::nullopt;
  Statement query(db, "SELECT s.pos, s.chinese_definition, s.english_definition "
                      "FROM words w JOIN senses s ON s.word_id=w.id WHERE w.word=? "
                      "ORDER BY s.order_idx, s.id LIMIT 1");
  query.bind(1, word);
  if (!query.step())
    return std::nullopt;
  const auto pos = columnText(query, 0);
  const auto chinese = columnText(query, 1);
  const auto english = query.text(2);
  return extractPosAndCleanGloss(pos, chinese ? &*chinese : nullptr, &english);
}
std::vector<RelatedWordItem> resolveDerivatives(sqlite3 *db,
                                                const std::vector<std::string> &family) {
  std::vector<RelatedWordItem> out;
  for (const auto &raw : family) {
    const auto [word, parsedPos, parsedGloss] = parseWordFamilyEntry(raw);
    if (word.empty())
      continue;
    if (parsedGloss && !trimCopy(*parsedGloss).empty()) {
      out.push_back({word, parsedPos, *parsedGloss});
      continue;
    }
    if (const auto found = lookupGloss(db, word)) {
      out.push_back({word, parsedPos ? parsedPos : found->first, found->second});
      continue;
    }
    if (const auto *fallback = fallbackRelatedGloss(normalizeWord(word)))
      out.push_back({word, parsedPos ? parsedPos
                                     : std::optional<std::string>(fallback->pos),
                     fallback->gloss});
    else
      out.push_back({word, parsedPos, {}});
  }
  return out;
}
std::vector<RelatedWordItem> resolveRelated(sqlite3 *db,
                                            const std::vector<std::string> &words) {
  std::vector<RelatedWordItem> out;
  for (const auto &raw : words) {
    const auto trimmed = trimCopy(raw);
    if (trimmed.empty())
      continue;
    if (const auto found = lookupGloss(db, trimmed)) {
      out.push_back({trimmed, found->first, found->second});
      continue;
    }
    const auto [word, parsedPos, parsedGloss] = parseWordFamilyEntry(trimmed);
    if (parsedPos && parsedGloss && !trimCopy(*parsedGloss).empty()) {
      out.push_back({word, parsedPos, *parsedGloss});
      continue;
    }
    if (const auto *fallback = fallbackRelatedGloss(normalizeWord(trimmed)))
      out.push_back({trimmed, std::string(fallback->pos), fallback->gloss});
    else
      out.push_back({trimmed, std::nullopt, {}});
  }
  return out;
}
std::vector<RelatedWordItem> prefixDerivatives(sqlite3 *db, const std::string &word,
                                              int limit) {
  if (utf8Length(word) < 4 || limit <= 0)
    return {};
  Statement query(
      db, "SELECT w.word, s.pos, s.chinese_definition, s.english_definition FROM words w "
          "JOIN senses s ON s.word_id=w.id AND s.order_idx=0 WHERE w.word>? AND w.word<? "
          "AND length(w.word) BETWEEN ? AND ? AND instr(w.word,' ')=0 AND instr(w.word,'-')=0 "
          "ORDER BY CASE WHEN w.frequency_rank IS NOT NULL AND w.frequency_rank>0 THEN 0 ELSE 1 END, "
          "w.frequency_rank, w.word LIMIT ?");
  query.bind(1, word)
      .bind(2, prefixUpperBound(word))
      .bind(3, static_cast<std::int64_t>(word.size() + 2))
      .bind(4, static_cast<std::int64_t>(word.size() + 7))
      .bind(5, static_cast<std::int64_t>(limit));
  std::vector<RelatedWordItem> rows;
  while (query.step()) {
    const auto pos = columnText(query, 1);
    const auto chinese = columnText(query, 2);
    const auto english = query.text(3);
    const auto cleaned =
        extractPosAndCleanGloss(pos, chinese ? &*chinese : nullptr, &english);
    rows.push_back({query.text(0), cleaned.first, cleaned.second});
  }
  return rows;
}
std::vector<RelatedWordItem> findLookalikes(sqlite3 *db, const std::string &word,
                                           int limit) {
  if (word.empty() || limit <= 0)
    return {};
  char32_t first = static_cast<unsigned char>(word[0]);
  std::size_t firstBytes = 1;
  if ((static_cast<unsigned char>(word[0]) & 0xE0) == 0xC0)
    firstBytes = 2;
  else if ((static_cast<unsigned char>(word[0]) & 0xF0) == 0xE0)
    firstBytes = 3;
  else if ((static_cast<unsigned char>(word[0]) & 0xF8) == 0xF0)
    firstBytes = 4;
  const std::string lower = word.substr(0, std::min(firstBytes, word.size()));
  (void)first;
  const auto chars = static_cast<std::int64_t>(utf8Length(word));
  Statement query(
      db, "SELECT w.id, w.word, w.frequency_rank FROM words w WHERE w.word>=? AND w.word<? "
          "AND length(w.word) BETWEEN ? AND ? AND w.word<>? ORDER BY CASE WHEN "
          "w.frequency_rank IS NOT NULL AND w.frequency_rank>0 THEN 0 ELSE 1 END, "
          "w.frequency_rank, w.word LIMIT 500");
  query.bind(1, lower)
      .bind(2, prefixUpperBound(lower))
      .bind(3, std::max<std::int64_t>(1, chars - 2))
      .bind(4, chars + 2)
      .bind(5, word);
  struct Candidate {
    std::size_t distance;
    std::size_t lengthGap;
    std::int64_t rank;
    std::int64_t id;
    std::string word;
  };
  std::vector<Candidate> matched;
  while (query.step()) {
    const auto candidate = query.text(1);
    const auto distance = levenshtein(word, candidate);
    if (distance < 1 || distance > 2)
      continue;
    const auto frequency = query.isNull(2) ? 0 : query.integer(2);
    matched.push_back({distance, utf8Length(candidate) > utf8Length(word)
                                     ? utf8Length(candidate) - utf8Length(word)
                                     : utf8Length(word) - utf8Length(candidate),
                       frequency > 0 ? frequency : std::numeric_limits<std::int64_t>::max(),
                       query.integer(0), candidate});
  }
  std::sort(matched.begin(), matched.end(), [](const Candidate &a, const Candidate &b) {
    if (a.distance != b.distance)
      return a.distance < b.distance;
    if (a.lengthGap != b.lengthGap)
      return a.lengthGap < b.lengthGap;
    if (a.rank != b.rank)
      return a.rank < b.rank;
    return a.word < b.word;
  });
  if (matched.size() > static_cast<std::size_t>(limit))
    matched.resize(static_cast<std::size_t>(limit));
  std::vector<RelatedWordItem> out;
  for (const auto &candidate : matched) {
    Statement sense(db, "SELECT pos, chinese_definition, english_definition FROM senses "
                        "WHERE word_id=? ORDER BY order_idx, id LIMIT 1");
    sense.bind(1, candidate.id);
    std::optional<std::string> pos;
    std::string gloss;
    if (sense.step()) {
      const auto chinese = columnText(sense, 1);
      const auto english = sense.text(2);
      const auto cleaned =
          extractPosAndCleanGloss(columnText(sense, 0), chinese ? &*chinese : nullptr,
                                  &english);
      pos = cleaned.first;
      gloss = cleaned.second;
    }
    out.push_back({candidate.word, pos, gloss});
  }
  return out;
}
std::string firstLine(const std::string &text) {
  const auto end = text.find_first_of("\r\n");
  return trimCopy(text.substr(0, end == std::string::npos ? text.size() : end));
}
std::optional<WordEntry> loadEntry(sqlite3 *db, std::int64_t id) {
  Statement word(db, "SELECT word, display, phonetic, frequency_rank, frequency_source, "
                     "synonyms, antonyms, word_family, tags, collins, oxford, source "
                     "FROM words WHERE id=?");
  word.bind(1, id);
  if (!word.step())
    return std::nullopt;
  WordEntry entry;
  entry.id = id;
  entry.word = word.text(0);
  entry.display = word.text(1);
  entry.phonetic = columnText(word, 2);
  if (!word.isNull(3))
    entry.frequency_rank = word.integer(3);
  if (entry.frequency_rank && *entry.frequency_rank <= 0)
    entry.frequency_rank.reset();
  const auto storedFrequencySource = columnText(word, 4);
  const auto synonyms = word.text(5);
  const auto antonyms = word.text(6);
  const auto family = word.text(7);
  auto tagsRaw = word.text(8);
  std::optional<std::int64_t> collins =
      word.isNull(9) ? std::nullopt : std::optional<std::int64_t>(word.integer(9));
  auto oxfordRaw = word.isNull(10) ? std::int64_t{0} : word.integer(10);
  const auto source = word.text(11);
  const SeedRecord *seed = source == "seed" ? findSeedRecord(entry.word) : nullptr;
  if (seed) {
    if (trimCopy(tagsRaw).empty())
      tagsRaw = seed->tags;
    if ((!collins || *collins <= 0) && seed->collins)
      collins = seed->collins;
    if (oxfordRaw == 0 && seed->oxford)
      oxfordRaw = 1;
  }
  if (collins && *collins <= 0)
    collins.reset();
  entry.tags = parseTags(tagsRaw);
  entry.collins = collins;
  entry.oxford = oxfordRaw != 0;
  for (const auto &tag : entry.tags)
    if (equalsIgnoreCase(tag, "oxford"))
      entry.oxford = true;
  if (entry.frequency_rank) {
    if (storedFrequencySource && !trimCopy(*storedFrequencySource).empty())
      entry.frequency_source = trimCopy(*storedFrequencySource);
    else if (source == "seed")
      entry.frequency_source = "seed_approx";
  }
  Statement senses(db, "SELECT id, pos, english_definition, chinese_definition, "
                       "level, level_source FROM senses WHERE word_id=? "
                       "ORDER BY order_idx, id");
  senses.bind(1, id);
  while (senses.step()) {
    Sense sense;
    sense.id = senses.integer(0);
    sense.pos = columnText(senses, 1);
    sense.english_definition = senses.text(2);
    sense.chinese_definition = columnText(senses, 3);
    const auto level = normalizeCefr(columnText(senses, 4));
    if (level) {
      sense.level = *level;
      const auto storedSource = columnText(senses, 5);
      if (storedSource && !trimCopy(*storedSource).empty())
        sense.level_source = trimCopy(*storedSource);
      else
        sense.level_source =
            source == "seed" ? "seed" : "legacy_unverified";
    }
    entry.senses.push_back(std::move(sense));
  }
  Statement examples(db, "SELECT e.id, e.sense_id, e.text, e.translation FROM examples e "
                         "JOIN senses s ON s.id=e.sense_id WHERE s.word_id=? "
                         "ORDER BY e.order_idx, e.id");
  examples.bind(1, id);
  std::vector<std::vector<Example>> grouped(entry.senses.size());
  while (examples.step()) {
    const auto senseId = examples.integer(1);
    Example example{examples.integer(0), examples.text(2), columnText(examples, 3)};
    for (std::size_t i = 0; i < entry.senses.size(); ++i)
      if (entry.senses[i].id == senseId)
        grouped[i].push_back(std::move(example));
  }
  for (std::size_t i = 0; i < entry.senses.size(); ++i)
    entry.senses[i].examples = std::move(grouped[i]);
  for (std::size_t i = 0; i < entry.senses.size(); ++i) {
    auto &sense = entry.senses[i];
    if (!sense.pos || trimCopy(*sense.pos).empty()) {
      const auto inferred = inferPosAndStrip(sense.english_definition, sense.chinese_definition);
      if (inferred.first)
        sense.pos = inferred.first;
      if (!inferred.second.empty())
        sense.english_definition = inferred.second;
    }
  }
  Statement collocations(db, "SELECT text, gloss FROM collocations WHERE word_id=? "
                             "ORDER BY order_idx, id");
  collocations.bind(1, id);
  while (collocations.step())
    entry.collocations.push_back({collocations.text(0), columnText(collocations, 1)});
  Statement forms(db, "SELECT kind, form FROM word_forms WHERE word_id=? ORDER BY rowid");
  forms.bind(1, id);
  while (forms.step())
    entry.forms.push_back(
        {formLabel(forms.text(0), entry.word, forms.text(1)), forms.text(1)});
  bool enrichment = false;
  {
    Statement extra(db, "SELECT mnemonic, roots_json, exam_example_text, "
                        "exam_example_translation, standard_example_text, "
                        "standard_example_translation FROM word_enrichments WHERE word_id=?");
    extra.bind(1, id);
    if (extra.step()) {
      enrichment = true;
      if (const auto mnemonic = columnText(extra, 0)) {
        const auto trimmed = trimCopy(*mnemonic);
        if (!trimmed.empty())
          entry.mnemonic = trimmed;
      }
      if (const auto roots = columnText(extra, 1)) {
        const auto trimmed = trimCopy(*roots);
        if (!trimmed.empty() && trimmed != "[]" && trimmed != "null") {
          try {
            for (const auto &item : Json::parse(trimmed))
              if (item.is_object() && item.contains("part") && item.contains("meaning") &&
                  item["part"].is_string() && item["meaning"].is_string())
                entry.roots.push_back({item["part"].get<std::string>(),
                                       item["meaning"].get<std::string>()});
          } catch (const Json::exception &) {
          }
        }
      }
      auto takeExample = [&](int textColumn, int translationColumn) {
        if (entry.exam_example)
          return;
        if (const auto text = columnText(extra, textColumn)) {
          const auto trimmed = trimCopy(*text);
          if (trimmed.empty())
            return;
          Example example;
          example.text = trimmed;
          if (const auto translation = columnText(extra, translationColumn)) {
            const auto value = trimCopy(*translation);
            if (!value.empty())
              example.translation = value;
          }
          entry.exam_example = example;
        }
      };
      takeExample(2, 3);
      takeExample(4, 5);
    }
  }
  if (!enrichment && seed) {
    entry.mnemonic = seed->mnemonic;
    entry.roots = seed->roots;
  }
  if (entry.roots.empty())
    entry.roots = decomposeRoots(entry.word);
  if (!entry.exam_example)
    for (const auto &sense : entry.senses) {
      if (!sense.examples.empty()) {
        entry.exam_example = sense.examples.front();
        break;
      }
    }
  entry.synonyms = parseStringList(synonyms);
  entry.antonyms = parseStringList(antonyms);
  entry.word_family = parseStringList(family);
  entry.derivatives = resolveDerivatives(db, entry.word_family);
  if (entry.derivatives.empty())
    entry.derivatives = prefixDerivatives(db, entry.word, 4);
  entry.synonyms_detailed = resolveRelated(db, entry.synonyms);
  entry.antonyms_detailed = resolveRelated(db, entry.antonyms);
  entry.lookalikes = findLookalikes(db, entry.word, 5);
  if (!entry.mnemonic && !entry.roots.empty()) {
    std::string breakdown;
    for (std::size_t i = 0; i < entry.roots.size(); ++i) {
      if (i)
        breakdown += " + ";
      breakdown += entry.roots[i].part + "（" + entry.roots[i].meaning + "）";
    }
    std::string primary;
    for (const auto &sense : entry.senses) {
      if (!sense.chinese_definition)
        continue;
      primary = firstLine(*sense.chinese_definition);
      break;
    }
    entry.mnemonic = primary.empty()
                         ? entry.display + " 构词拆解：" + breakdown + "。"
                         : entry.display + " 构词记忆：" + breakdown + " → " + primary + "。";
  }
  return entry;
}
} // namespace
std::optional<WordEntry> dictionaryById(sqlite3 *db, std::int64_t id) {
  return loadEntry(db, id);
}
std::optional<WordEntry> dictionaryFind(sqlite3 *db, const std::string &input) {
  const auto word = normalizeWord(input);
  if (word.empty())
    return std::nullopt;
  if (const auto id = wordIdWhere(db, "SELECT id FROM words WHERE word=?", word))
    return loadEntry(db, *id);
  const auto pattern = "word:" + ftsQuoteTerm(word);
  if (const auto id = wordIdWhere(db,
                                  "SELECT s.word_id FROM words_fts f JOIN senses s ON s.id=f.rowid "
                                  "WHERE words_fts MATCH ?1 LIMIT 1",
                                  pattern))
    return loadEntry(db, *id);
  if (const auto id = wordIdWhere(db,
                                  "SELECT word_id FROM word_forms WHERE form=? ORDER BY word_id LIMIT 1",
                                  word))
    return loadEntry(db, *id);
  return std::nullopt;
}
std::vector<Suggestion> dictionarySuggest(sqlite3 *db, const std::string &input, int limit) {
  const auto query = normalizeWord(input);
  std::vector<Suggestion> rows;
  if (query.empty() || limit <= 0)
    return rows;
  const auto bound = prefixUpperBound(query);
  {
    Statement ranked(db, "SELECT word, display, phonetic, frequency_rank FROM words "
                         "WHERE word>=? AND word<? AND frequency_rank IS NOT NULL "
                         "ORDER BY frequency_rank LIMIT ?");
    ranked.bind(1, query).bind(2, bound).bind(3, static_cast<std::int64_t>(limit));
    while (ranked.step())
      rows.push_back(readSuggestion(ranked));
  }
  if (rows.size() < static_cast<std::size_t>(limit)) {
    std::unordered_set<std::string> seen;
    for (const auto &row : rows)
      seen.insert(row.word);
    Statement alpha(db, "SELECT word, display, phonetic, frequency_rank FROM words "
                        "WHERE word>=? AND word<? ORDER BY word LIMIT ?");
    alpha.bind(1, query).bind(2, bound).bind(3, static_cast<std::int64_t>(limit));
    while (alpha.step()) {
      auto suggestion = readSuggestion(alpha);
      if (!seen.insert(suggestion.word).second)
        continue;
      rows.push_back(std::move(suggestion));
      if (rows.size() >= static_cast<std::size_t>(limit))
        break;
    }
  }
  std::stable_sort(rows.begin(), rows.end(), [&](const Suggestion &left, const Suggestion &right) {
    return (left.word != query) < (right.word != query);
  });
  if (rows.size() > static_cast<std::size_t>(limit))
    rows.resize(static_cast<std::size_t>(limit));
  return rows;
}
} // namespace lexica::parity
