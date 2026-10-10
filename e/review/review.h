#pragma once
#include "common/result.h"
#include "common/stage.h"
#include "common/types.h"
#include <string>
#include <vector>

class Dict;
class Vocab;

struct Question {
    QuestionId id = 0;
    SessionId sessionId;
    EntryId entryId = 0;
    std::string prompt;
    std::string kind;
};

struct NextQuestion {
    bool has = false;
    Question question;
};

struct Hint {
    int level = 0;
    std::string text;
};

struct Grade {
    EntryId entryId = 0;
    bool correct = false;
    bool duplicate = false;
    int stage = 0;
    int hintLevel = 0;
};

struct Summary {
    int formalCount = 0;
    int formalCorrect = 0;
    int practiceCount = 0;
    std::vector<std::string> reinforce;
};

class Review : public StageSource {
public:
    void open(const std::string& dataDir);
    void close();
    void bind(Dict* dict, Vocab* vocab);
    ErrorCode ensureState(EntryId id);
    int stageOf(EntryId id) const override;
    void countStages(int& stage0, int& stage1, int& stage2, int& stage3) const;
    Result<SessionId> start();
    Result<NextQuestion> next(const SessionId& sid);
    Result<Hint> hint(QuestionId qid, int level);
    Result<Grade> submit(QuestionId qid, const std::string& answer);
    Result<Summary> summary(const SessionId& sid);

private:
    struct State {
        EntryId id = 0;
        int stage = 0;
    };
    struct Item {
        QuestionId id = 0;
        SessionId sessionId;
        EntryId entryId = 0;
        std::string answer;
        std::string prompt;
        std::string definition;
        std::string translation;
        int hintLevel = 0;
        bool graded = false;
        bool correct = false;
    };
    struct Session {
        SessionId id;
        std::vector<Item> items;
    };

    std::string dataDir_;
    Dict* dict_ = nullptr;
    Vocab* vocab_ = nullptr;
    std::vector<State> states_;
    std::vector<Session> sessions_;
    QuestionId nextQid_ = 1;
    int nextSid_ = 1;

    State* findState(EntryId id);
    const State* findState(EntryId id) const;
    Item* findItem(QuestionId qid);
    Session* findSession(const SessionId& sid);
    Question toQuestion(const Item& item) const;
    std::string hintText(const Item& item, int level) const;
};
