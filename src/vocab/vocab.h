// 个人词汇模块：保存收藏、笔记、暂停和活动记录，提供列表与统计。
#pragma once
#include "common/result.h"
#include "common/stage.h"
#include "common/types.h"
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

// 在调用方已经打开的事务里写入正式复习事件；题目编号相同则不能插第二条。
bool insertReviewHistory(sqlite3* db, EntryId id, QuestionId qid, std::int64_t utc);

// 列表筛选与分页条件；favorite、paused、stage 为 -1 时不按该项筛选。
struct VocabQuery {
    int favorite = -1;
    int stage = -1;
    int paused = -1;
    std::string q;
    int page = 1;
    int size = kPageDefault;
};

// “我的词汇”中的一行：单词、收藏、笔记、暂停和阶段。
struct VocabRow {
    EntryId entryId = 0;
    std::string word;
    bool favorite = false;
    std::string note;
    bool paused = false;
    int stage = 0;
};

// 一页个人词汇；total 是筛选后的总条数，items 是当前页内容。
struct VocabPage {
    int total = 0;
    std::vector<VocabRow> items;
};

// 一次查词事件，只存词条编号和 UTC 秒时间。
struct HistoryItem {
    EntryId entryId = 0;
    std::int64_t utc = 0;
};

// 一页查词历史；total 是事件数，不是去重后的单词数。
struct HistoryPage {
    int total = 0;
    std::vector<HistoryItem> items;
};

// 某个上海自然日的查词次数和正式复习次数。
struct DayCount {
    std::string date;
    int lookups = 0;
    int reviews = 0;
};

// 统计页数据：14 天活动、各阶段词数和暂停数量。
struct Stats {
    std::vector<DayCount> days;
    int stage0 = 0;
    int stage1 = 0;
    int stage2 = 0;
    int stage3 = 0;
    int paused = 0;
};

// 个人词汇与活动记录入口，数据保存在共享 SQLite 库中。
class Vocab {
public:
    Vocab() = default;
    ~Vocab();
    Vocab(const Vocab&) = delete;
    Vocab& operator=(const Vocab&) = delete;

    // 打开词库模块已建好的共享库并检查个人词汇表；失败时保持连接为空。
    void open(const std::string& dataDir);
    // 关闭数据库连接，释放本模块持有的资源。
    void close();
    // 用 opId 去重；首次请求同时写个人词汇记录和查词事件。
    ErrorCode recordLookup(EntryId id, const std::string& word, const OpId& opId);
    // 设置或取消收藏；同一个词共用一条个人词汇记录。
    ErrorCode setFavorite(EntryId id, const std::string& word, bool value);
    // 保存笔记；超过 5000 个字符时拒绝，不覆盖原笔记。
    ErrorCode setNote(EntryId id, const std::string& word, const std::string& text);
    // 设置是否暂停复习；暂停后选题时会跳过这个词。
    ErrorCode setPaused(EntryId id, const std::string& word, bool value);
    // 筛选收藏、暂停、词头和学习阶段，再返回当前页及筛选后的总数。
    Result<VocabPage> list(const VocabQuery& query, const StageSource* stages);
    // 读取正式查词历史，按最近发生的记录优先排列并分页。
    Result<HistoryPage> history(int page, int size);
    // 按上海日期汇总查词和复习事件，输出含今天的 14 天，空白日期补 0。
    Result<Stats> stats();
    // 追加一次正式复习事件，供最近 14 天活动统计使用。
    ErrorCode addReview(EntryId id);
    // 按题目补记正式复习；同一题已有记录时直接成功，不增加次数。
    ErrorCode addReview(EntryId id, QuestionId qid);
    // 读取这个词是否暂停；复习模块选题时通过它过滤。
    bool isPaused(EntryId id) const;
    // 统计暂停复习的词条数量。
    int pausedCount() const;

private:
    std::string dataDir_;
    sqlite3* db_ = nullptr;

    // 确保词条在个人词汇表中有一行，并同步其词头。
    bool ensureItem(EntryId id, const std::string& word);
    // 收藏和暂停共用的写入过程：先建记录，再在事务内更新开关。
    ErrorCode setFlag(EntryId id, const std::string& word, const char* column, bool value);
};
