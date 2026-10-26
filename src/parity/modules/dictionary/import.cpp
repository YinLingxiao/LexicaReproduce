#include "parity/modules/dictionary/api.h"
#include <memory>
#include <optional>
namespace lexica::parity {
namespace {
struct Stmt {
  sqlite3 *db;
  sqlite3_stmt *statement = nullptr;
  Stmt(sqlite3 *database, const char *sql) : db(database) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
      throw Error("DB_ERROR", sqlite3_errmsg(db));
  }
  ~Stmt() { sqlite3_finalize(statement); }
  Stmt(const Stmt &) = delete;
  void check(int code) const {
    if (code != SQLITE_OK)
      throw Error("DB_ERROR", sqlite3_errmsg(db));
  }
  void bindText(int index, const std::string &value) {
    check(sqlite3_bind_text(statement, index, value.data(),
                            static_cast<int>(value.size()), SQLITE_TRANSIENT));
  }
  void bindNull(int index) { check(sqlite3_bind_null(statement, index)); }
  void bindInt(int index, std::int64_t value) {
    check(sqlite3_bind_int64(statement, index, value));
  }
  void reset() {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
  }
  bool next() {
    throwIfCancelled(currentCancellation());
    const int code = sqlite3_step(statement);
    if (code == SQLITE_ROW)
      return true;
    if (code == SQLITE_DONE)
      return false;
    throwIfCancelled(currentCancellation());
    throw Error("DB_ERROR", sqlite3_errmsg(db));
  }
  void run() {
    throwIfCancelled(currentCancellation());
    const int code = sqlite3_step(statement);
    if (code != SQLITE_DONE && code != SQLITE_ROW)
      throw Error("DB_ERROR", sqlite3_errmsg(db));
    reset();
  }
  bool isNull(int index) const {
    return sqlite3_column_type(statement, index) == SQLITE_NULL;
  }
  std::string text(int index) const {
    const auto value = sqlite3_column_text(statement, index);
    return value ? std::string(reinterpret_cast<const char *>(value),
                               sqlite3_column_bytes(statement, index))
                 : std::string{};
  }
  std::optional<std::string> maybe(int index) const {
    return isNull(index) ? std::nullopt
                         : std::optional<std::string>(text(index));
  }
  std::optional<std::int64_t> maybeInt(int index) const {
    return isNull(index) ? std::nullopt
                         : std::optional<std::int64_t>(
                               sqlite3_column_int64(statement, index));
  }
};
struct Closer {
  sqlite3 *db = nullptr;
  ~Closer() {
    if (db)
      sqlite3_close(db);
  }
};
std::vector<std::string> splitLines(const std::optional<std::string> &value) {
  std::vector<std::string> lines;
  if (!value)
    return lines;
  const auto &text = *value;
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t end = index;
    while (end < text.size() && text[end] != '\n' && text[end] != '\r')
      ++end;
    const auto line = trimCopy(text.substr(index, end - index));
    if (!line.empty())
      lines.push_back(line);
    if (end < text.size() && text[end] == '\r')
      ++end;
    if (end < text.size() && text[end] == '\n')
      ++end;
    index = end;
  }
  return lines;
}
bool restAllowsPos(const std::string &rest) {
  if (rest.empty() || rest.find_first_not_of(" \t\r\n") != 0)
    return true;
  const auto ch = static_cast<unsigned char>(rest[0]);
  return ch < 'A' || (ch > 'Z' && ch < 'a') || ch > 'z';
}
std::optional<std::string>
extractSensePos(const std::string &english,
                const std::optional<std::string> &chinese,
                const std::optional<std::string> &rawPos) {
  const char *prefixes[] = {"n.",    "vt.",   "vi.",   "v.",   "adj.", "adv.",
                            "prep.", "conj.", "pron.", "art.", "num.", "int."};
  const std::string *texts[] = {&english, chinese ? &*chinese : nullptr};
  for (const std::string *text : texts) {
    if (!text)
      continue;
    std::size_t index = 0;
    while (index <= text->size()) {
      std::size_t end = index;
      while (end < text->size() && (*text)[end] != '\n' && (*text)[end] != '\r')
        ++end;
      const auto line = trimCopy(text->substr(index, end - index));
      for (const char *prefix : prefixes)
        if (line.rfind(prefix, 0) == 0 &&
            restAllowsPos(line.substr(std::char_traits<char>::length(prefix))))
          return std::string(prefix);
      if (line.rfind("a.", 0) == 0 && restAllowsPos(line.substr(2)))
        return std::string("adj.");
      if (end >= text->size())
        break;
      if ((*text)[end] == '\r')
        ++end;
      if (end < text->size() && (*text)[end] == '\n')
        ++end;
      index = end;
    }
  }
  if (!rawPos)
    return std::nullopt;
  auto segment = *rawPos;
  const auto slash = segment.find('/');
  if (slash != std::string::npos)
    segment.resize(slash);
  const auto colon = segment.find(':');
  if (colon == std::string::npos)
    return std::nullopt;
  const auto code = trimCopy(segment.substr(0, colon));
  if (code == "n")
    return "n.";
  if (code == "v")
    return "v.";
  if (code == "j" || code == "a")
    return "adj.";
  if (code == "r")
    return "adv.";
  if (code == "p")
    return "prep.";
  if (code == "c")
    return "conj.";
  return std::nullopt;
}
const char *defaultLevel(const std::optional<std::int64_t> &collins,
                         std::int64_t oxford,
                         const std::optional<std::int64_t> &rank) {
  const auto value = collins.value_or(0);
  if (value >= 4 || (oxford > 0 && rank && *rank > 0 && *rank <= 2000))
    return "B1";
  if (value == 3 || oxford > 0 || (rank && *rank > 0 && *rank <= 6000))
    return "B2";
  if (value >= 1 || (rank && *rank > 0 && *rank <= 15000))
    return "C1";
  return "R";
}
} // namespace
OwnedConnection openConfiguredDatabase(const std::filesystem::path &path) {
  sqlite3 *db = nullptr;
  const auto code = sqlite3_open_v2(path.u8string().c_str(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                        SQLITE_OPEN_FULLMUTEX,
                                    nullptr);
  if (code != SQLITE_OK) {
    const std::string message =
        db ? sqlite3_errmsg(db) : "Cannot open database";
    if (db)
      sqlite3_close(db);
    throw Error("DB_ERROR", message);
  }
  OwnedConnection owned(db);
  sqlite3_progress_handler(
      db, 1000,
      [](void *) -> int {
        const auto *token = currentCancellation();
        return token && token->requested() ? 1 : 0;
      },
      nullptr);
  sqlite3_busy_timeout(db, 2000);
  execute(db, "PRAGMA foreign_keys=ON");
  execute(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  return owned;
}
std::int64_t importEcdictDatabase(sqlite3 *dest,
                                  const std::filesystem::path &sourcePath,
                                  const std::string &createdAt,
                                  const EcdictImportHooks &hooks) {
  throwIfCancelled(currentCancellation());
  Closer source;
  const auto opened = sqlite3_open_v2(sourcePath.u8string().c_str(), &source.db,
                                      SQLITE_OPEN_READONLY, nullptr);
  if (opened != SQLITE_OK) {
    const std::string message =
        source.db ? sqlite3_errmsg(source.db) : "Cannot open ECDICT source";
    throw Error("DB_ERROR", message);
  }
  sqlite3_progress_handler(
      source.db, 1000,
      [](void *) -> int {
        const auto *token = currentCancellation();
        return token && token->requested() ? 1 : 0;
      },
      nullptr);
  Stmt table(source.db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                        "AND name='stardict'");
  if (!table.next() || table.maybeInt(0).value_or(0) == 0)
    throw Error("RESOURCE_ERROR",
                "not an ECDICT stardict.db (table `stardict` missing)");
  Stmt select(source.db, "SELECT word, phonetic, definition, translation, frq, "
                         "bnc, exchange, pos, collins, "
                         "oxford, tag FROM stardict ORDER BY (COALESCE(frq, 0) "
                         "> 0 OR COALESCE(bnc, 0) > 0) "
                         "DESC, id");
  Stmt insertWord(
      dest, "INSERT OR IGNORE INTO words(word, display, phonetic, "
            "frequency_rank, synonyms, "
            "antonyms, word_family, tags, collins, oxford, source, created_at) "
            "VALUES(?,?,?,?,'[]','[]','[]',?,?,?,'ecdict',?)");
  Stmt insertSense(dest, "INSERT INTO senses(word_id, pos, english_definition, "
                         "chinese_definition, "
                         "level, order_idx) VALUES(?,?,?,?,?,?)");
  Stmt insertForm(
      dest,
      "INSERT OR IGNORE INTO word_forms(form, word_id, kind) VALUES(?,?,?)");
  const auto batchSize =
      hooks.batchSize == 0 ? kEcdictImportBatch : hooks.batchSize;
  std::int64_t imported = 0;
  std::size_t inChunk = 0;
  auto tx = std::make_unique<Transaction>(dest);
  while (select.next()) {
    const auto raw = trimCopy(select.maybe(0).value_or(""));
    if (raw.empty())
      continue;
    const auto word = normalizeWord(raw);
    if (word.empty())
      continue;
    const auto englishLines = splitLines(select.maybe(2));
    const auto chineseLines = splitLines(select.maybe(3));
    if (englishLines.empty() && chineseLines.empty())
      continue;
    std::optional<std::string> phonetic;
    if (const auto rawPhonetic = select.maybe(1)) {
      const auto trimmed = trimCopy(*rawPhonetic);
      if (!trimmed.empty())
        phonetic = trimmed.front() == '/' ? trimmed : "/" + trimmed + "/";
    }
    const auto frequency = select.maybeInt(4);
    const auto bnc = select.maybeInt(5);
    const std::optional<std::int64_t> rank = frequency && *frequency > 0
                                                 ? frequency
                                             : bnc && *bnc > 0 ? bnc
                                                               : std::nullopt;
    auto collins = select.maybeInt(8);
    if (collins && *collins <= 0)
      collins.reset();
    const auto oxford =
        select.maybeInt(9).value_or(0) > 0 ? std::int64_t{1} : std::int64_t{0};
    const auto tags = trimCopy(select.maybe(10).value_or(""));
    insertWord.bindText(1, word);
    insertWord.bindText(2, raw);
    if (phonetic)
      insertWord.bindText(3, *phonetic);
    else
      insertWord.bindNull(3);
    if (rank)
      insertWord.bindInt(4, *rank);
    else
      insertWord.bindNull(4);
    insertWord.bindText(5, tags);
    if (collins)
      insertWord.bindInt(6, *collins);
    else
      insertWord.bindNull(6);
    insertWord.bindInt(7, oxford);
    insertWord.bindText(8, createdAt);
    insertWord.run();
    if (sqlite3_changes(dest) == 0)
      continue;
    ++imported;
    ++inChunk;
    const auto wordId = sqlite3_last_insert_rowid(dest);
    std::vector<std::pair<std::string, std::optional<std::string>>> senses;
    if (englishLines.size() == chineseLines.size() && englishLines.size() > 1) {
      for (std::size_t i = 0; i < englishLines.size(); ++i)
        senses.emplace_back(englishLines[i], chineseLines[i]);
    } else {
      std::string english;
      for (std::size_t i = 0; i < englishLines.size(); ++i) {
        if (i)
          english.push_back('\n');
        english += englishLines[i];
      }
      std::optional<std::string> chinese;
      if (!chineseLines.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < chineseLines.size(); ++i) {
          if (i)
            joined.push_back('\n');
          joined += chineseLines[i];
        }
        chinese = joined;
      }
      senses.emplace_back(std::move(english), std::move(chinese));
    }
    const auto level = defaultLevel(collins, oxford, rank);
    const auto rawPos = select.maybe(7);
    for (std::size_t i = 0; i < senses.size(); ++i) {
      const auto pos =
          extractSensePos(senses[i].first, senses[i].second, rawPos);
      insertSense.bindInt(1, wordId);
      if (pos)
        insertSense.bindText(2, *pos);
      else
        insertSense.bindNull(2);
      insertSense.bindText(3, senses[i].first);
      if (senses[i].second)
        insertSense.bindText(4, *senses[i].second);
      else
        insertSense.bindNull(4);
      insertSense.bindText(5, level);
      insertSense.bindInt(6, static_cast<std::int64_t>(i));
      insertSense.run();
    }
    if (const auto exchange = select.maybe(6)) {
      std::size_t index = 0;
      const auto &text = *exchange;
      while (index < text.size()) {
        const auto end = text.find('/', index);
        const auto token = text.substr(
            index, end == std::string::npos ? std::string::npos : end - index);
        const auto colon = token.find(':');
        if (colon != std::string::npos) {
          const auto form = normalizeWord(token.substr(colon + 1));
          const char *mapped = mapFormKind(token.substr(0, colon));
          const char *kind = mapped ? mapped : "form";
          if (!form.empty() && form != word) {
            insertForm.bindText(1, form);
            insertForm.bindInt(2, wordId);
            insertForm.bindText(3, kind);
            insertForm.run();
          }
        }
        if (end == std::string::npos)
          break;
        index = end + 1;
      }
    }
    if (inChunk >= batchSize) {
      if (hooks.beforeCommit)
        hooks.beforeCommit();
      throwIfCancelled(currentCancellation());
      tx->commit();
      tx.reset();
      inChunk = 0;
      if (hooks.betweenBatches)
        hooks.betweenBatches();
      tx = std::make_unique<Transaction>(dest);
    }
  }
  throwIfCancelled(currentCancellation());
  tx->commit();
  return imported;
}
} // namespace lexica::parity
