// 应用编排模块：把查词、个人词汇、复习和 AI 串起来，供网页和控制台调用。
#pragma once
#include "ai/ai.h"
#include "dict/dict.h"
#include "review/review.h"
#include "vocab/vocab.h"
#include <string>

// 业务总入口：界面只需调用它，由它协调各模块。
class App {
public:
    // 依次打开词库、个人词汇、复习和 AI，并接好复习所需的回调。
    StartupStatus start(const std::string& dataDir);
    // 关闭复习、词汇和词库连接，释放数据库资源。
    void shutdown();
    // 返回启动状态；词库导入后，空库状态会随之变成正常。
    StartupStatus status() const;
    // 查询词库中是否已经有可用词条。
    bool dictReady() const;

    // 正式查词：先查词库模块，命中后让个人词汇模块记历史，再让复习模块初始化学习状态。
    Result<Entry> lookup(const std::string& word, const OpId& opId);
    // 按输入前缀取最多 8 个候选词；这一步不算正式查词。
    Result<std::vector<SuggestItem>> suggest(const std::string& prefix);
    // 按编号读取词条，只读数据，不记历史。
    Result<Entry> entry(EntryId id);
    // 同步导入词库，调用者要等导入结束后才能取得结果。
    Result<ImportReport> importDict(const std::string& path);
    // 启动后台导入并立即返回任务；已有导入在运行时返回当前任务。
    Result<ImportTask> importStart(const std::string& path);
    // 按任务编号查询导入状态和累计处理数量。
    Result<ImportTask> importTask(const std::string& id) const;

    // 设置或取消收藏；同一个词共用一条个人词汇记录。
    ErrorCode setFavorite(EntryId id, bool value);
    // 保存笔记；超过 5000 个字符时拒绝，不覆盖原笔记。
    ErrorCode setNote(EntryId id, const std::string& text);
    // 设置是否暂停复习；暂停后选题时会跳过这个词。
    ErrorCode setPaused(EntryId id, bool value);
    // 把筛选条件交给个人词汇模块，并让个人词汇模块从 复习模块读取每个词的阶段。
    Result<VocabPage> vocabList(const VocabQuery& query);
    // 读取正式查词历史，按最近发生的记录优先排列并分页。
    Result<HistoryPage> history(int page, int size);
    // 先取个人词汇模块的活动统计，再补上复习模块的阶段词数和暂停数量。
    Result<Stats> stats();

    // 让复习模块按到期时间创建一轮复习，返回这轮的编号。
    Result<SessionId> reviewStart();
    // 取得这一轮尚未作答的下一道题。
    Result<NextQuestion> reviewNext(const SessionId& sid);
    // 请求指定题目的提示；level=0 表示取下一级。
    Result<Hint> reviewHint(QuestionId qid, int level);
    // 交给复习模块判分；正式题补记复习次数，同一题只记一次，重练不计数。
    Result<Grade> reviewSubmit(QuestionId qid, const std::string& answer);
    // 读取这一轮的正式成绩、重练次数和待巩固单词。
    Result<Summary> reviewSummary(const SessionId& sid);

    // 读取 AI 配置，只返回是否配了密钥，不返回密钥内容。
    AiConfig aiConfig() const;
    // 保存 AI 开关、服务地址和模型名称。
    ErrorCode aiSave(bool enabled, const std::string& baseUrl, const std::string& model);
    // 发起一次简短 AI 请求，用来检查连接和配置。
    ErrorCode aiTest();
    // 先检查词条与单词是否对应，再请求生成或读取缓存例句。
    Result<AiTask> aiExample(EntryId id, const std::string& word);
    // 查询后台 AI 任务的状态和例句结果。
    Result<AiTask> aiTask(const std::string& id);

private:
    std::string dataDir_;
    StartupStatus status_ = StartupStatus::DictEmpty;
    Dict dict_;
    Vocab vocab_;
    Review review_;
    Ai ai_;

    // 创建数据目录，并写入一个临时文件，确认目录确实可以保存数据。
    bool ensureDir(const std::string& path);
    // 先确认词条存在；成功时把完整词条放进输出参数 entry。
    ErrorCode requireEntry(EntryId id, Entry& entry);
    // 优先使用英文释义；没有英文释义时改用中文翻译。
    static std::string meaningOf(const Entry& entry);
};
