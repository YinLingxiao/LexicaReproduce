#pragma once
#include "common/result.h"
#include "common/stage.h"
#include "common/types.h"
#include "common/clock.h"
#include <cstdint>
#include <functional>
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
    bool practice = false;
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
    bool practice = false;
};

struct Summary {
    int formalCount = 0;
    int formalCorrect = 0;
    int practiceCount = 0;
    std::vector<std::string> reinforce;
};

using SentenceSource = std::function<std::string(EntryId id, const std::string& word)>;

class Review : public StageSource {
public:
    Review() = default;
    ~Review();
    Review(const Review&) = delete;
    Review& operator=(const Review&) = delete;

    bool open(const std::string& dataDir);
    void close();
    void bind(Dict* dict, Vocab* vocab);
    void setSentenceSource(SentenceSource source);
    // 测试注入假时钟（分派 §七.9：时间用 Clock，测试时换假时钟，不要真的等几天）。
    // 正常运行时无需调用，默认使用 systemClock()；不改动任何既有公开函数签名。
    void setClock(const Clock* clock);
    ErrorCode ensureState(EntryId id);
    int stageOf(EntryId id) const override;
    void countStages(int& stage0, int& stage1, int& stage2, int& stage3) const;
    Result<SessionId> start();
    Result<NextQuestion> next(const SessionId& sid);
    Result<Hint> hint(QuestionId qid, int level);
    Result<Grade> submit(QuestionId qid, const std::string& answer);
    Result<Summary> summary(const SessionId& sid);

private:
    struct Item {
        QuestionId id = 0;
        SessionId sessionId;
        EntryId entryId = 0;
        std::string answer;
        std::string prompt;
        std::string extra;
        std::string kind;
        int hintLevel = 0;
        int gradedHint = 0;
        int resultStage = 0;
        bool practice = false;
        bool graded = false;
        bool correct = false;
    };
    struct Session {
        SessionId id;
        std::vector<Item> items;
    };

    std::string dataDir_;
    void* db_ = nullptr;
    Dict* dict_ = nullptr;
    Vocab* vocab_ = nullptr;
    SentenceSource sentences_;
    const Clock* clock_ = nullptr;      // nullptr → systemClock()
    std::vector<Session> sessions_;
    QuestionId nextQid_ = 1;
    std::int64_t nextSid_ = 1;

    std::int64_t nowUtc() const;
    Item* findItem(QuestionId qid);
    Session* findSession(const SessionId& sid);
    Question toQuestion(const Item& item) const;
    std::string hintText(const Item& item, int level) const;
    // 成绩 + 学习状态在同一事务内落盘（分派 §七.7）；updateState=false 表示重练题，
    // 只记成绩不改状态（分派 §七.6）
    bool persist(const Item& item, int stage, std::int64_t due, bool updateState);
    bool buildItem(EntryId id, bool practice, Item& item);
};
