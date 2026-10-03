#include "review/review.h"
#include "dict/dict.h"
#include "vocab/vocab.h"
#include "common/text.h"
#include <algorithm>

void Review::open(const std::string& dataDir) { dataDir_ = dataDir; }

void Review::close() {}

void Review::bind(Dict* dict, Vocab* vocab) {
    dict_ = dict;
    vocab_ = vocab;
}

Review::State* Review::findState(EntryId id) {
    for (State& state : states_) {
        if (state.id == id) return &state;
    }
    return nullptr;
}

const Review::State* Review::findState(EntryId id) const {
    for (const State& state : states_) {
        if (state.id == id) return &state;
    }
    return nullptr;
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

ErrorCode Review::ensureState(EntryId id) {
    if (id <= 0) return ErrorCode::BadRequest;
    if (!findState(id)) {
        State state;
        state.id = id;
        states_.push_back(state);
    }
    return ErrorCode::Ok;
}

int Review::stageOf(EntryId id) const {
    const State* state = findState(id);
    return state ? state->stage : 0;
}

void Review::countStages(int& stage0, int& stage1, int& stage2, int& stage3) const {
    stage0 = stage1 = stage2 = stage3 = 0;
    for (const State& state : states_) {
        if (state.stage <= 0) ++stage0;
        else if (state.stage == 1) ++stage1;
        else if (state.stage == 2) ++stage2;
        else ++stage3;
    }
}

Question Review::toQuestion(const Item& item) const {
    Question question;
    question.id = item.id;
    question.sessionId = item.sessionId;
    question.entryId = item.entryId;
    question.prompt = item.prompt;
    question.kind = "local";
    return question;
}

Result<SessionId> Review::start() {
    if (!dict_ || !vocab_) return Result<SessionId>::fail(ErrorCode::Internal);
    std::vector<EntryId> due;
    for (const State& state : states_) {
        if (vocab_->isPaused(state.id)) continue;
        due.push_back(state.id);
    }
    std::sort(due.begin(), due.end());
    Session session;
    session.id = "s" + std::to_string(nextSid_++);
    for (EntryId id : due) {
        if (static_cast<int>(session.items.size()) >= kReviewFormalMax) break;
        Result<Entry> entry = dict_->get(id);
        if (!entry.ok()) continue;
        if (entry.value.translation.empty() && entry.value.definition.empty()) continue;
        Item item;
        item.id = nextQid_++;
        item.sessionId = session.id;
        item.entryId = id;
        item.answer = entry.value.word;
        item.prompt = entry.value.translation.empty() ? entry.value.definition : entry.value.translation;
        item.definition = entry.value.definition;
        item.translation = entry.value.translation;
        session.items.push_back(item);
    }
    sessions_.push_back(session);
    return Result<SessionId>::success(sessions_.back().id);
}

Result<NextQuestion> Review::next(const SessionId& sid) {
    Session* session = findSession(sid);
    if (!session) return Result<NextQuestion>::fail(ErrorCode::QuestionExpired);
    NextQuestion next;
    for (const Item& item : session->items) {
        if (item.graded) continue;
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
    if (level == 2) return item.definition;
    return item.translation;
}

Result<Hint> Review::hint(QuestionId qid, int level) {
    Item* item = findItem(qid);
    if (!item) return Result<Hint>::fail(ErrorCode::QuestionExpired);
    if (level == 0) level = item->hintLevel < 3 ? item->hintLevel + 1 : 3;
    if (level < 1 || level > 3) return Result<Hint>::fail(ErrorCode::BadRequest);
    if (level > item->hintLevel) item->hintLevel = level;
    Hint hint;
    hint.level = level;
    hint.text = hintText(*item, level);
    return Result<Hint>::success(hint);
}

Result<Grade> Review::submit(QuestionId qid, const std::string& answer) {
    Item* item = findItem(qid);
    if (!item) return Result<Grade>::fail(ErrorCode::QuestionExpired);
    Grade grade;
    grade.entryId = item->entryId;
    grade.stage = stageOf(item->entryId);
    grade.hintLevel = item->hintLevel;
    if (item->graded) {
        grade.correct = item->correct;
        grade.duplicate = true;
        return Result<Grade>::success(grade);
    }
    item->graded = true;
    item->correct = !item->answer.empty() && normalizeWord(answer) == item->answer;
    grade.correct = item->correct;
    return Result<Grade>::success(grade);
}

Result<Summary> Review::summary(const SessionId& sid) {
    Session* session = findSession(sid);
    if (!session) return Result<Summary>::fail(ErrorCode::QuestionExpired);
    Summary summary;
    summary.formalCount = static_cast<int>(session->items.size());
    for (const Item& item : session->items) {
        if (!item.graded) continue;
        if (item.correct) ++summary.formalCorrect;
        else summary.reinforce.push_back(item.answer);
    }
    return Result<Summary>::success(summary);
}
