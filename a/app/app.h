#pragma once
#include "ai/ai.h"
#include "dict/dict.h"
#include "review/review.h"
#include "vocab/vocab.h"
#include <string>

class App {
public:
    StartupStatus start(const std::string& dataDir);
    void shutdown();
    StartupStatus status() const;
    bool dictReady() const;

    Result<Entry> lookup(const std::string& word, const OpId& opId);
    Result<std::vector<SuggestItem>> suggest(const std::string& prefix);
    Result<Entry> entry(EntryId id);
    Result<ImportReport> importDict(const std::string& path);

    ErrorCode setFavorite(EntryId id, bool value);
    ErrorCode setNote(EntryId id, const std::string& text);
    ErrorCode setPaused(EntryId id, bool value);
    Result<VocabPage> vocabList(const VocabQuery& query);
    Result<HistoryPage> history(int page, int size);
    Result<Stats> stats();

    Result<SessionId> reviewStart();
    Result<NextQuestion> reviewNext(const SessionId& sid);
    Result<Hint> reviewHint(QuestionId qid, int level);
    Result<Grade> reviewSubmit(QuestionId qid, const std::string& answer);
    Result<Summary> reviewSummary(const SessionId& sid);

    AiConfig aiConfig() const;
    ErrorCode aiSave(bool enabled, const std::string& baseUrl, const std::string& model);
    ErrorCode aiTest();
    Result<AiTask> aiExample(EntryId id, const std::string& word);
    Result<AiTask> aiTask(const std::string& id);

private:
    std::string dataDir_;
    StartupStatus status_ = StartupStatus::DictEmpty;
    Dict dict_;
    Vocab vocab_;
    Review review_;
    Ai ai_;

    bool ensureDir(const std::string& path);
    ErrorCode requireEntry(EntryId id, Entry& entry);
};
