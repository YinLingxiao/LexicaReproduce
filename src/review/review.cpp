// 复习模块：按到期时间选题、给提示、判分、安排下次复习，并保存成绩。
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

// 拼出 dataDir 下共享数据库 lexica.db 的路径。
std::string dbPath(const std::string& dir) {
    if (dir.empty()) return "lexica.db";
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + "lexica.db";
    return dir + "/lexica.db";
}

// 执行不需要逐行取结果的 SQL，并返回是否成功。
bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

// 把阶段 0/1/2/3 转成下次复习间隔 1/3/7/14 天。
int intervalDays(int stage) {
    if (stage <= 0) return 1;
    if (stage == 1) return 3;
    if (stage == 2) return 7;
    return 14;
}

// 只遮住完整目标词，不遮住其他单词中碰巧相同的字母。
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

// 依次遮住目标词及其词形，防止题干直接给出答案。
std::string maskWords(std::string text, const std::vector<std::string>& words) {
    for (const std::string& word : words) text = maskWord(text, word);
    return text;
}

// 检查文本是否还含有目标词或其任一词形。
bool containsAny(const std::string& text, const std::vector<std::string>& words) {
    for (const std::string& word : words) {
        if (containsWholeWord(text, word)) return true;
    }
    return false;
}

// 读取当前毫秒时间，用作任务或题目编号的起点。
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

Review::~Review() { close(); }

// 打开复习数据库连接，检查状态表，并创建成绩表。
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
    // 成绩表（复习模块自己的数据表，分派 §七.7）：幂等自建，不登记词库模块的迁移体系，
    // 避免触发词库模块的 VersionTooNew 检查；如需纳入迁移找词库模块登记 review v2。
    if (!exec(db, "CREATE TABLE IF NOT EXISTS review_result ("
                  "qid INTEGER PRIMARY KEY,"
                  "entry_id INTEGER NOT NULL,"
                  "session_id TEXT NOT NULL,"
                  "correct INTEGER NOT NULL,"
                  "practice INTEGER NOT NULL DEFAULT 0,"
                  "utc INTEGER NOT NULL)")) {
        sqlite3_close(db);
        return false;
    }
    db_ = db;
    return true;
}

// 关闭数据库连接，释放本模块持有的资源。
void Review::close() {
    sessions_.clear();
    if (!db_) return;
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
}

// 保存词库和个人词汇模块的指针，供出题与检查暂停状态使用。
void Review::bind(Dict* dict, Vocab* vocab) {
    dict_ = dict;
    vocab_ = vocab;
}

// 接入应用编排模块提供的例句获取函数；有可用例句时可出 AI 填空题。
void Review::setSentenceSource(SentenceSource source) { sentences_ = std::move(source); }

// 设置测试时钟；传 nullptr 时使用真实系统时间。
void Review::setClock(const Clock* clock) { clock_ = clock; }

// 优先读取注入的测试时钟；未注入时读取真实系统时间。
std::int64_t Review::nowUtc() const {
    return clock_ ? clock_->nowUtc() : systemClock().nowUtc();
}

// 首次接触单词时创建阶段 0、一天后到期的状态；已有状态不改。
ErrorCode Review::ensureState(EntryId id) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!db_) return ErrorCode::DbError;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "INSERT OR IGNORE INTO review_state(entry_id, stage, due_utc) VALUES(?,0,?)", -1, &st, nullptr) != SQLITE_OK) {
        return ErrorCode::DbError;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, nowUtc() + 86400);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? ErrorCode::Ok : ErrorCode::DbError;
}

// 读取一个词当前的学习阶段；没有记录时返回 0。
int Review::stageOf(EntryId id) const {
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT stage FROM review_state WHERE entry_id=?", -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, id);
    int stage = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return stage;
}

// 按阶段分组统计词数，供统计页面展示。
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

// 把内部题目转成可返回给界面的数据，去掉答案和内部判分状态。
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

// 读取释义并遮词；优先尝试缓存的 AI 例句，否则使用本地释义出题。
bool Review::buildItem(EntryId id, bool practice, Item& item) {
    Result<Entry> entry = dict_->get(id);
    if (!entry.ok()) return false;
    const std::string& definition = entry.value.definition;
    const std::string& translation = entry.value.translation;
    if (definition.empty() && translation.empty()) return false;
    std::vector<std::string> words = dict_->forms(entry.value.word);
    words.insert(words.begin(), entry.value.word);
    // 先遮较长词形，减少短词先替换后影响长词匹配的情况。
    std::sort(words.begin(), words.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    item.entryId = id;
    item.answer = entry.value.word;
    item.practice = practice;
    item.kind.clear();
    // 这里只读取缓存例句，不等待网络；拿不到就走下面的本地释义题。
    std::string sentence = !practice && sentences_ ? sentences_(id, entry.value.word) : "";
    if (!sentence.empty()) {
        std::string masked = maskWords(sentence, words);
        if (masked != sentence && !containsAny(masked, words)) {
            item.prompt = masked;
            item.extra = maskWords(translation.empty() ? definition : translation, words);
            item.kind = "ai";
        }
    }
    // AI 题无法使用时，优先取英文释义出本地题，没有英文才用中文。
    if (item.kind.empty()) {
        bool useDefinition = !definition.empty();
        item.prompt = maskWords(useDefinition ? definition : translation, words);
        item.extra = maskWords(useDefinition ? translation : definition, words);
        item.kind = "local";
    }
    // 最后再检查一次题干，仍有目标词或词形就放弃这道题。
    if (containsAny(item.prompt, words)) return false;
    item.id = nextQid_++;
    return true;
}

// 读出题时的阶段和到期时间，供正式提交做版本检查。
bool Review::readState(EntryId id, int& stage, std::int64_t& due) const {
    if (!db_ || id <= 0) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT stage, due_utc FROM review_state WHERE entry_id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    const bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        stage = sqlite3_column_int(st, 0);
        due = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return ok;
}

// 同一词只保留最新一轮里尚未作答的正式题。
void Review::supersedeFormal(EntryId id) {
    for (Session& session : sessions_) {
        for (Item& item : session.items) {
            if (!item.practice && !item.graded && item.entryId == id) item.superseded = true;
        }
    }
}

// 按到期时间和词条编号选题，跳过暂停词，每轮最多 12 道正式题。
// 新的一轮会使同一词尚未作答的旧正式题失效；正式提交还要核对出题时的阶段和到期时间。
// 重练题留在原会话里，不因新开一轮而失效，也不改学习状态。
Result<SessionId> Review::start() {
    if (!dict_ || !vocab_) return Result<SessionId>::fail(ErrorCode::Internal);
    if (!db_) return Result<SessionId>::fail(ErrorCode::DbError);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT entry_id FROM review_state WHERE due_utc<=? ORDER BY due_utc, entry_id", -1, &st, nullptr) != SQLITE_OK) {
        return Result<SessionId>::fail(ErrorCode::DbError);
    }
    sqlite3_bind_int64(st, 1, nowUtc());
    // 先按到期时间、编号取候选，再过滤暂停词和无法出题的词。
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
        if (!readState(id, item.seenStage, item.seenDue)) continue;
        supersedeFormal(id);
        item.sessionId = session.id;
        session.items.push_back(item);
    }
    sessions_.push_back(session);
    // 只保留最近 20 轮；更早的会话和题号会失效。
    if (sessions_.size() > kSessionKeep) sessions_.erase(sessions_.begin());
    return Result<SessionId>::success(session.id);
}

// 在保留的会话中按题号寻找内部题目，找不到返回 nullptr。
Review::Item* Review::findItem(QuestionId qid) {
    for (Session& session : sessions_) {
        for (Item& item : session.items) {
            if (item.id != qid) continue;
            if (item.superseded && !item.graded) return nullptr;
            return &item;
        }
    }
    return nullptr;
}

// 按会话编号寻找一轮复习，找不到表示会话已失效。
Review::Session* Review::findSession(const SessionId& sid) {
    for (Session& session : sessions_) {
        if (session.id == sid) return &session;
    }
    return nullptr;
}

// 取本轮第一道未提交且未暂停的题；没有题时 has=false。
Result<NextQuestion> Review::next(const SessionId& sid) {
    Session* session = findSession(sid);
    if (!session) return Result<NextQuestion>::fail(ErrorCode::QuestionExpired);
    NextQuestion next;
    for (const Item& item : session->items) {
        if (item.graded || item.superseded) continue;
        if (vocab_ && vocab_->isPaused(item.entryId)) continue;
        next.has = true;
        next.question = toQuestion(item);
        return Result<NextQuestion>::success(next);
    }
    return Result<NextQuestion>::success(next);
}

// 生成提示：第一级首字母，第二级另一种释义，第三级直接给答案。
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

// 返回提示并记住使用过的最高级别，让后续判分知道是否看过答案。
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

// 成绩、学习状态和正式复习次数在同一事务内落盘：
//   updateState=true  → 按出题时的阶段和到期时间更新 review_state，并写入成绩与复习次数
//   updateState=false → 仅 INSERT review_result，practice 标记为 1（重练题，不改正式成绩）
Review::PersistCode Review::persist(const Item& item, int stage, std::int64_t due, bool updateState, bool correct) {
    sqlite3* db = static_cast<sqlite3*>(db_);
    if (!exec(db, "BEGIN IMMEDIATE")) return PersistCode::Error;
    PersistCode result = PersistCode::Ok;
    if (updateState) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE review_state SET stage=?, due_utc=? WHERE entry_id=? AND stage=? AND due_utc=? RETURNING entry_id", -1, &st, nullptr) != SQLITE_OK) {
            result = PersistCode::Error;
        } else {
            sqlite3_bind_int(st, 1, stage);
            sqlite3_bind_int64(st, 2, due);
            sqlite3_bind_int64(st, 3, item.entryId);
            sqlite3_bind_int(st, 4, item.seenStage);
            sqlite3_bind_int64(st, 5, item.seenDue);
            const int step = sqlite3_step(st);
            if (step == SQLITE_ROW) {
                if (sqlite3_step(st) != SQLITE_DONE) result = PersistCode::Error;
            } else if (step == SQLITE_DONE) {
                result = PersistCode::Conflict;
            } else {
                result = PersistCode::Error;
            }
        }
        if (st) sqlite3_finalize(st);
    }
    if (result == PersistCode::Ok) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT INTO review_result(qid, entry_id, session_id, correct, practice, utc) VALUES(?,?,?,?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
            result = PersistCode::Error;
        } else {
            sqlite3_bind_int64(st, 1, item.id);
            sqlite3_bind_int64(st, 2, item.entryId);
            sqlite3_bind_text(st, 3, item.sessionId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 4, correct ? 1 : 0);
            sqlite3_bind_int(st, 5, item.practice ? 1 : 0);
            sqlite3_bind_int64(st, 6, nowUtc());
            if (sqlite3_step(st) != SQLITE_DONE) result = PersistCode::Error;
        }
        if (st) sqlite3_finalize(st);
    }
    if (result == PersistCode::Ok && updateState && !insertReviewHistory(db, item.entryId, item.id, nowUtc())) result = PersistCode::Error;
    if (result != PersistCode::Ok || !exec(db, "COMMIT")) {
        exec(db, "ROLLBACK");
        if (result == PersistCode::Ok) result = PersistCode::Error;
        return result;
    }
    return PersistCode::Ok;
}

// 匹配答案、计算新阶段并保存；正式题未掌握时追加一次重练。
Result<Grade> Review::submit(QuestionId qid, const std::string& answer) {
    Item* item = findItem(qid);
    if (!item) return Result<Grade>::fail(ErrorCode::QuestionExpired);
    if (!db_) return Result<Grade>::fail(ErrorCode::DbError);
    Grade grade;
    grade.entryId = item->entryId;
    grade.practice = item->practice;
    // 同一题再次提交时复用已算出的结果，避免同一题重复计分。
    if (item->graded) {
        grade.correct = item->correct;
        grade.duplicate = true;
        grade.stage = item->resultStage;
        grade.hintLevel = item->gradedHint;
        return Result<Grade>::success(grade);
    }
    // 输入去两端空白并忽略大小写；看过第三级答案即使填对也算未掌握。
    bool matched = !item->answer.empty() && normalizeWord(answer) == normalizeWord(item->answer);
    bool mastered = matched && item->hintLevel < 3;
    int nextStage = 0;
    // 正式题影响阶段：无提示答对升级，用过提示答对不变，未掌握降级。
    // 数据库事务成功后才记下判分，失败时这道题还可以重试。
    if (!item->practice) {
        nextStage = item->seenStage;
        if (mastered && item->hintLevel == 0) nextStage = std::min(item->seenStage + 1, 3);
        else if (!mastered) nextStage = std::max(item->seenStage - 1, 0);
        // 根据答题后的阶段决定间隔，从当前时间起计算下次到期时间。
        const std::int64_t due = nowUtc() + static_cast<std::int64_t>(intervalDays(nextStage)) * 86400;
        const PersistCode saved = persist(*item, nextStage, due, true, mastered);
        if (saved == PersistCode::Conflict) {
            item->superseded = true;
            return Result<Grade>::fail(ErrorCode::QuestionExpired);
        }
        if (saved != PersistCode::Ok) return Result<Grade>::fail(ErrorCode::DbError);
    } else {
        nextStage = stageOf(item->entryId);
        if (persist(*item, nextStage, 0, false, mastered) != PersistCode::Ok) return Result<Grade>::fail(ErrorCode::DbError);
    }
    item->graded = true;
    item->correct = mastered;
    item->gradedHint = item->hintLevel;
    item->resultStage = nextStage;
    grade.correct = mastered;
    grade.stage = nextStage;
    grade.hintLevel = item->gradedHint;
    if (item->practice || mastered) return Result<Grade>::success(grade);
    // 正式题未掌握时，在这一轮末尾加一道重练；同词只追加一次。
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

// 汇总已作答题目；重练单独计数，重练答对后移出待巩固列表。
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
        // 正式答错的词，如果后续重练已经答对，就不再放进待巩固列表。
        bool practicedOk = false;
        for (const Item& other : session->items) {
            if (other.practice && other.entryId == item.entryId && other.graded && other.correct) practicedOk = true;
        }
        if (!practicedOk) summary.reinforce.push_back(item.answer);
    }
    return Result<Summary>::success(summary);
}
