#pragma once
#include "common/result.h"
#include "common/stage.h"
#include "common/types.h"
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

struct VocabQuery {
    int favorite = -1;
    int stage = -1;
    int paused = -1;
    std::string q;
    int page = 1;
    int size = kPageDefault;
};

struct VocabRow {
    EntryId entryId = 0;
    std::string word;
    bool favorite = false;
    std::string note;
    bool paused = false;
    int stage = 0;
};

struct VocabPage {
    int total = 0;
    std::vector<VocabRow> items;
};

struct HistoryItem {
    EntryId entryId = 0;
    std::int64_t utc = 0;
};

struct HistoryPage {
    int total = 0;
    std::vector<HistoryItem> items;
};

struct DayCount {
    std::string date;
    int lookups = 0;
    int reviews = 0;
};

struct Stats {
    std::vector<DayCount> days;
    int stage0 = 0;
    int stage1 = 0;
    int stage2 = 0;
    int stage3 = 0;
    int paused = 0;
};

class Vocab {
public:
    Vocab() = default;
    ~Vocab();
    Vocab(const Vocab&) = delete;
    Vocab& operator=(const Vocab&) = delete;

    void open(const std::string& dataDir);
    void close();
    ErrorCode recordLookup(EntryId id, const std::string& word, const OpId& opId);
    ErrorCode setFavorite(EntryId id, const std::string& word, bool value);
    ErrorCode setNote(EntryId id, const std::string& word, const std::string& text);
    ErrorCode setPaused(EntryId id, const std::string& word, bool value);
    Result<VocabPage> list(const VocabQuery& query, const StageSource* stages);
    Result<HistoryPage> history(int page, int size);
    Result<Stats> stats();
    ErrorCode addReview(EntryId id);
    bool isPaused(EntryId id) const;
    int pausedCount() const;

private:
    std::string dataDir_;
    sqlite3* db_ = nullptr;

    bool ensureItem(EntryId id, const std::string& word);
    ErrorCode setFlag(EntryId id, const std::string& word, const char* column, bool value);
};
