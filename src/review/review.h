// 复习模块：按到期时间选题、给提示、判分、安排下次复习，并保存成绩。
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

// 返回给界面的题目，只含题干等公开信息，不含答案。
struct Question {
    QuestionId id = 0;
    SessionId sessionId;
    EntryId entryId = 0;
    std::string prompt;
    std::string kind;
    bool practice = false;
};

// has 表示还有没有题；只有为 true 时 question 才有效。
struct NextQuestion {
    bool has = false;
    Question question;
};

// 提示结果：当前请求的提示级别和提示文本。
struct Hint {
    int level = 0;
    std::string text;
};

// 提交结果：是否掌握、是否重复提交、阶段及提示使用情况。
struct Grade {
    EntryId entryId = 0;
    bool correct = false;
    bool duplicate = false;
    int stage = 0;
    int hintLevel = 0;
    bool practice = false;
};

// 本轮总结：正式题成绩、重练次数和仍需巩固的词。
struct Summary {
    int formalCount = 0;
    int formalCorrect = 0;
    int practiceCount = 0;
    std::vector<std::string> reinforce;
};

using SentenceSource = std::function<std::string(EntryId id, const std::string& word)>;

// 复习调度入口；学习状态和成绩落盘，会话题目保存在内存中。
class Review : public StageSource {
public:
    Review() = default;
    ~Review();
    Review(const Review&) = delete;
    Review& operator=(const Review&) = delete;

    // 打开复习数据库连接，检查状态表，并创建成绩表。
    bool open(const std::string& dataDir);
    // 关闭数据库连接，释放本模块持有的资源。
    void close();
    // 保存词库和个人词汇模块的指针，供出题与检查暂停状态使用。
    void bind(Dict* dict, Vocab* vocab);
    // 接入应用编排模块提供的例句获取函数；有可用例句时可出 AI 填空题。
    void setSentenceSource(SentenceSource source);
    // 测试注入假时钟（分派 §七.9：时间用 Clock，测试时换假时钟，不要真的等几天）。
    // 正常运行时无需调用，默认使用 systemClock()；不改动任何既有公开函数签名。
    void setClock(const Clock* clock);
    // 首次接触单词时创建阶段 0、一天后到期的状态；已有状态不改。
    ErrorCode ensureState(EntryId id);
    // 读取一个词当前的学习阶段；没有记录时返回 0。
    int stageOf(EntryId id) const override;
    // 按阶段分组统计词数，供统计页面展示。
    void countStages(int& stage0, int& stage1, int& stage2, int& stage3) const;
    // 按到期时间和词条编号选题，跳过暂停词，每轮最多 12 道正式题。
    Result<SessionId> start();
    // 取本轮第一道未提交且未暂停的题；没有题时 has=false。
    Result<NextQuestion> next(const SessionId& sid);
    // 返回提示并记住使用过的最高级别，让后续判分知道是否看过答案。
    Result<Hint> hint(QuestionId qid, int level);
    // 匹配答案、计算新阶段并保存；正式题未掌握时追加一次重练。
    Result<Grade> submit(QuestionId qid, const std::string& answer);
    // 汇总已作答题目；重练单独计数，重练答对后移出待巩固列表。
    Result<Summary> summary(const SessionId& sid);

private:
    // 内部题目：除了公开题干，还保存答案、提示和判分状态。
    struct Item {
        QuestionId id = 0;
        SessionId sessionId;
        EntryId entryId = 0;
        std::string answer;
        std::string prompt;
        std::string extra;
        std::string kind;
        // hintLevel 是当前最高提示；gradedHint 和 resultStage 保存首次判分结果供重试复用。
        int hintLevel = 0;
        int gradedHint = 0;
        int resultStage = 0;
        // 出题时看到的阶段和到期时间；正式提交必须仍是这一版，避免两轮重复升级。
        int seenStage = 0;
        std::int64_t seenDue = 0;
        bool practice = false;
        // graded 表示已经提交过；correct 在这里表示是否掌握，包含“看过答案算未掌握”的规则。
        bool graded = false;
        bool correct = false;
        // 新的一轮已经拿走这个到期状态，旧正式题不能再计分。
        bool superseded = false;
    };
    // 一轮复习：会话编号及该轮的正式题和重练题。
    struct Session {
        SessionId id;
        std::vector<Item> items;
    };

    std::string dataDir_;
    void* db_ = nullptr;
    // 借用 App 中的模块对象，只保存地址，不在这里销毁它们。
    Dict* dict_ = nullptr;
    Vocab* vocab_ = nullptr;
    SentenceSource sentences_;
    const Clock* clock_ = nullptr;      // nullptr → systemClock()
    std::vector<Session> sessions_;
    QuestionId nextQid_ = 1;
    std::int64_t nextSid_ = 1;

    enum class PersistCode { Ok, Conflict, Error };

    // 优先读取注入的测试时钟；未注入时读取真实系统时间。
    std::int64_t nowUtc() const;
    // 读出题时的阶段和到期时间，供正式提交做版本检查。
    bool readState(EntryId id, int& stage, std::int64_t& due) const;
    // 同一词只保留最新一轮里尚未作答的正式题。
    void supersedeFormal(EntryId id);
    // 在保留的会话中按题号寻找内部题目，找不到返回 nullptr。
    Item* findItem(QuestionId qid);
    // 按会话编号寻找一轮复习，找不到表示会话已失效。
    Session* findSession(const SessionId& sid);
    // 把内部题目转成可返回给界面的数据，去掉答案和内部判分状态。
    Question toQuestion(const Item& item) const;
    // 生成提示：第一级首字母，第二级另一种释义，第三级直接给答案。
    std::string hintText(const Item& item, int level) const;
    // 成绩、学习状态和正式复习次数在同一事务内落盘；updateState=false 表示重练题，
    // 只记成绩不改状态、不增加复习次数。状态版本不符时返回 Conflict。
    PersistCode persist(const Item& item, int stage, std::int64_t due, bool updateState, bool correct);
    // 读取释义并遮词；优先尝试缓存的 AI 例句，否则使用本地释义出题。
    bool buildItem(EntryId id, bool practice, Item& item);
};
