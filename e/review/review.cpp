#include "review/review.h"
#include "dict/dict.h"
#include "vocab/vocab.h"
#include "common/clock.h"
#include "common/text.h"
#include "sqlite3.h"
#include <algorithm>
#include <cctype>
#include <chrono>

namespace {

const std::size_t kSessionKeep = 20;

std::string dbPath(const std::string& dir) {
    if (dir.empty()) return "lexica.db";
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + "lexica.db";
    return dir + "/lexica.db";
}

bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

int intervalDays(int stage) {
    if (stage <= 0) return 1;
    if (stage == 1) return 3;
    if (stage == 2) return 7;
    return 14;
}

std::string maskWord(const std::string& text, const std::string& word) {
    std::string needle = normalizeWord(word);
    if (needle.empty()) return text;
    std::string lower;
    lower.reserve(text.size());
    for (unsigned char c : text) lower.push_back(static_cast<char>(std::tolower(c)));
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (i + needle.size() <= lower.size() && lower.compare(i, needle.size(), needle) == 0) {
            bool left = i == 0 || !std::isalpha(static_cast<unsigned char>(lower[i - 1]));
            std::size_t j = i + needle.size();
            bool right = j >= lower.size() || !std::isalpha(static_cast<unsigned char>(lower[j]));
            if (left && right) {
                out.append(needle.size(), '_');
                i = j;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::string maskWords(std::string text, const std::vector<std::string>& words) {
    for (const std::string& word : words) text = maskWord(text, word);
    return text;
}

bool containsAny(const std::string& text, const std::vector<std::string>& words) {
    for (const std::string& word : words) {
        if (containsWholeWord(text, word)) return true;
    }
    return false;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

Review::~Review() { close(); }

bool Review::open(const std::string& dataDir) {
    close();
    dataDir_ = dataDir;
    nextQid_ = nowMs() * 1000;
    nextSid_ = nextQid_;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath(dataDir).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='review_state'", -1, &st, nullptr) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_ROW;
    if (st) sqlite3_finalize(st);
    if (!ok) {
        sqlite3_close(db);
        return false;
    }
    db_ = db;
    return true;
}

void Review::close() {
    sessions_.clear();
    if (!db_) return;
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
}

void Review::bind(Dict* dict, Vocab* vocab) {
    dict_ = dict;
    vocab_ = vocab;
}

void Review::setSentenceSource(SentenceSource source) { sentences_ = std::move(source); }

ErrorCode Review::ensureState(EntryId id) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "INSERT OR IGNORE INTO review_state(entry_id, stage, due_utc) VALUES(?,0,?)", -1, &st, nullptr) != SQLITE_OK) {
        return ErrorCode::DbError;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, systemClock().nowUtc() + 86400);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? ErrorCode::Ok : ErrorCode::DbError;
}

int Review::stageOf(EntryId id) const {
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT stage FROM review_state WHERE entry_id=?", -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, id);
    int stage = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return stage;
}

void Review::countStages(int& stage0, int& stage1, int& stage2, int& stage3) const {
    stage0 = stage1 = stage2 = stage3 = 0;
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT stage, COUNT(*) FROM review_state GROUP BY stage", -1, &st, nullptr) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int stage = sqlite3_column_int(st, 0);
        int count = sqlite3_column_int(st, 1);
        if (stage <= 0) stage0 += count;
        else if (stage == 1) stage1 += count;
        else if (stage == 2) stage2 += count;
        else stage3 += count;
    }
    sqlite3_finalize(st);
}

Question Review::toQuestion(const Item& item) const {
    Question question;
    question.id = item.id;
    question.sessionId = item.sessionId;
    question.entryId = item.entryId;
    question.prompt = item.prompt;
    question.kind = item.kind;
    question.practice = item.practice;
    return question;
}

bool Review::buildItem(EntryId id, bool practice, Item& item) {
    Result<Entry> entry = dict_->get(id);
    if (!entry.ok()) return false;
    const std::string& definition = entry.value.definition;
    const std::string& translation = entry.value.translation;
    if (definition.empty() && translation.empty()) return false;
    std::vector<std::string> words = dict_->forms(entry.value.word);
    words.insert(words.begin(), entry.value.word);
    std::sort(words.begin(), words.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    item.entryId = id;
    item.answer = entry.value.word;
    item.practice = practice;
    item.kind.clear();
    std::string sentence = !practice && sentences_ ? sentences_(id, entry.value.word) : "";
    if (!sentence.empty()) {
        std::string masked = maskWords(sentence, words);
        if (masked != sentence && !containsAny(masked, words)) {
            item.prompt = masked;
            item.extra = maskWords(translation.empty() ? definition : translation, words);
            item.kind = "ai";
        }
    }
    if (item.kind.empty()) {
        bool useDefinition = !definition.empty();
        item.prompt = maskWords(useDefinition ? definition : translation, words);
        item.extra = maskWords(useDefinition ? translation : definition, words);
        item.kind = "local";
    }
    if (containsAny(item.prompt, words)) return false;
    item.id = nextQid_++;
    return true;
}

Result<SessionId> Review::start() {
    if (!dict_ || !vocab_) return Result<SessionId>::fail(ErrorCode::Internal);
    if (!db_) return Result<SessionId>::fail(ErrorCode::DbError);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT entry_id FROM review_state WHERE due_utc<=? ORDER BY due_utc, entry_id", -1, &st, nullptr) != SQLITE_OK) {
        return Result<SessionId>::fail(ErrorCode::DbError);
    }
    sqlite3_bind_int64(st, 1, systemClock().nowUtc());
    std::vector<EntryId> due;
    while (sqlite3_step(st) == SQLITE_ROW) due.push_back(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    Session session;
    session.id = "s" + std::to_string(nextSid_++);
    for (EntryId id : due) {
        if (static_cast<int>(session.items.size()) >= kReviewFormalMax) break;
        if (vocab_->isPaused(id)) continue;
        Item item;
        if (!buildItem(id, false, item)) continue;
        item.sessionId = session.id;
        session.items.push_back(item);
    }
    sessions_.push_back(session);
    if (sessions_.size() > kSessionKeep) sessions_.erase(sessions_.begin());
    return Result<SessionId>::success(session.id);
}

Review::Item* Review::findItem(QuestionId qid) {
    for (Session& session : sessions_) {
        for (Item& item : session.items) {
            if (item.id == qid) return &item;
        }
    }
    return nullptr;
}

Review::Session* Review::findSession(const SessionId& sid) {
    for (Session& session : sessions_) {
        if (session.id == sid) return &session;
    }
    return nullptr;
}

Result<NextQuestion> Review::next(const SessionId& sid) {
    Session* session = findSession(sid);
    if (!session) return Result<NextQuestion>::fail(ErrorCode::QuestionExpired);
    NextQuestion next;
    for (const Item& item : session->items) {
        if (item.graded) continue;
        if (vocab_ && vocab_->isPaused(item.entryId)) continue;
        next.has = true;
        next.question = toQuestion(item);
        return Result<NextQuestion>::success(next);
    }
    return Result<NextQuestion>::success(next);
}

std::string Review::hintText(const Item& item, int level) const {
    if (level <= 1) {
        if (item.answer.empty()) return "";
        std::string text(1, item.answer[0]);
        if (item.answer.size() > 1) text.append(item.answer.size() - 1, '_');
        return text;
    }
    if (level == 2) return item.extra;
    return item.answer;
}

Result<Hint> Review::hint(QuestionId qid, int level) {
    Item* item = findItem(qid);
    if (!item) return Result<Hint>::fail(ErrorCode::QuestionExpired);
    if (level == 0) level = item->hintLevel < 3 ? item->hintLevel + 1 : 3;
    if (level < 1 || level > 3) return Result<Hint>::fail(ErrorCode::BadRequest);
    if (!item->graded && level > item->hintLevel) item->hintLevel = level;
    Hint hint;
    hint.level = level;
    hint.text = hintText(*item, level);
    return Result<Hint>::success(hint);
}

bool Review::writeState(EntryId id, int stage, std::int64_t due) {
    sqlite3* db = static_cast<sqlite3*>(db_);
    if (!exec(db, "BEGIN IMMEDIATE")) return false;
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(db, "UPDATE review_state SET stage=?, due_utc=? WHERE entry_id=?", -1, &st, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int(st, 1, stage);
        sqlite3_bind_int64(st, 2, due);
        sqlite3_bind_int64(st, 3, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    if (st) sqlite3_finalize(st);
    if (!ok || !exec(db, "COMMIT")) {
        exec(db, "ROLLBACK");
        return false;
    }
    return true;
}

Result<Grade> Review::submit(QuestionId qid, const std::string& answer) {
    Item* item = findItem(qid);
    if (!item) return Result<Grade>::fail(ErrorCode::QuestionExpired);
    if (!db_) return Result<Grade>::fail(ErrorCode::DbError);
    Grade grade;
    grade.entryId = item->entryId;
    grade.practice = item->practice;
    if (item->graded) {
        grade.correct = item->correct;
        grade.duplicate = true;
        grade.stage = item->resultStage;
        grade.hintLevel = item->gradedHint;
        return Result<Grade>::success(grade);
    }
    bool matched = !item->answer.empty() && normalizeWord(answer) == normalizeWord(item->answer);
    bool mastered = matched && item->hintLevel < 3;
    int stage = stageOf(item->entryId);
    int nextStage = stage;
    if (!item->practice) {
        if (mastered && item->hintLevel == 0) nextStage = std::min(stage + 1, 3);
        else if (!mastered) nextStage = std::max(stage - 1, 0);
        std::int64_t due = systemClock().nowUtc() + static_cast<std::int64_t>(intervalDays(nextStage)) * 86400;
        if (!writeState(item->entryId, nextStage, due)) return Result<Grade>::fail(ErrorCode::DbError);
    }
    item->graded = true;
    item->correct = mastered;
    item->gradedHint = item->hintLevel;
    item->resultStage = nextStage;
    grade.correct = mastered;
    grade.stage = nextStage;
    grade.hintLevel = item->gradedHint;
    if (item->practice || mastered) return Result<Grade>::success(grade);
    EntryId entryId = item->entryId;
    SessionId sid = item->sessionId;
    Session* session = findSession(sid);
    if (!session) return Result<Grade>::success(grade);
    for (const Item& other : session->items) {
        if (other.practice && other.entryId == entryId) return Result<Grade>::success(grade);
    }
    Item practice;
    if (buildItem(entryId, true, practice)) {
        practice.sessionId = sid;
        session->items.push_back(practice);
    }
    return Result<Grade>::success(grade);
}

Result<Summary> Review::summary(const SessionId& sid) {
    Session* session = findSession(sid);
    if (!session) return Result<Summary>::fail(ErrorCode::QuestionExpired);
    Summary summary;
    for (const Item& item : session->items) {
        if (!item.graded) continue;
        if (item.practice) {
            ++summary.practiceCount;
            continue;
        }
        ++summary.formalCount;
        if (item.correct) {
            ++summary.formalCorrect;
            continue;
        }
        bool practicedOk = false;
        for (const Item& other : session->items) {
            if (other.practice && other.entryId == item.entryId && other.graded && other.correct) practicedOk = true;
        }
        if (!practicedOk) summary.reinforce.push_back(item.answer);
    }
    return Result<Summary>::success(summary);
}
