// 词库模块：管理共享数据库的建表升级、词库导入、精确查词和词形回退。
#pragma once
#include "common/result.h"
#include "common/types.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

// 启动检查的结果，用来区分空库、目录不可写、坏库和版本过新。
enum class StartupStatus {
    Ok,
    DictEmpty,
    DirNotWritable,
    DbCorrupt,
    VersionTooNew
};

// 把启动状态转成网页和控制台使用的固定名称。
const char* startupName(StartupStatus status);

// 完整词条；query 保存用户查询的词形，word 是命中的词头。
struct Entry {
    EntryId id = 0;
    std::string word;
    std::string phonetic;
    std::string translation;
    std::string definition;
    std::string query;
};

// 输入联想的一条候选，只需要词条编号和单词。
struct SuggestItem {
    EntryId id = 0;
    std::string word;
};

// 导入计数：新增、更新、跳过和无效记录数。
struct ImportReport {
    int added = 0;
    int updated = 0;
    int skipped = 0;
    int failed = 0;
};

// 后台导入任务：编号、运行状态、计数和错误码。
struct ImportTask {
    std::string id;
    std::string status;
    ImportReport report;
    ErrorCode error = ErrorCode::Ok;
};

// 词库查询与导入入口，同时负责共享库的版本迁移。
class Dict {
public:
    Dict() = default;
    ~Dict();
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;

    // 打开源词库并检查必要的表或字段，成功后准备逐条读取。
    StartupStatus open(const std::string& dataDir);
    // 关闭数据库连接，释放本模块持有的资源。
    void close();
    // 返回词库是否有词条；后台导入也会更新这个状态。
    bool ready() const;
    // 先按规范化词头精确查找；找不到时通过词形表回退到原型。
    Result<Entry> lookup(const std::string& word);
    // 按输入前缀取最多 8 个候选词；这一步不算正式查词。
    Result<std::vector<SuggestItem>> suggest(const std::string& prefix);
    // 按词条编号读取完整内容。
    Result<Entry> get(EntryId id);
    // 读取这个原型词对应的全部词形，供复习题遮住答案使用。
    std::vector<std::string> forms(const std::string& word);
    // 同步读取词库并写入数据库，完成后刷新词库是否可用的状态。
    Result<ImportReport> importFile(const std::string& path);
    // 启动后台导入并立即返回任务；已有导入在运行时返回当前任务。
    Result<ImportTask> importStart(const std::string& path);
    // 按任务编号查询导入状态和累计处理数量。
    Result<ImportTask> importTask(const std::string& id) const;

private:
    std::string dataDir_;
    std::string dbPath_;
    sqlite3* db_ = nullptr;
    // atomic 允许主线程和导入线程安全地读写这两个开关。
    std::atomic<bool> ready_{false};
    std::thread worker_;
    std::atomic<bool> stop_{false};
    // 导入进度是多字段数据，用锁保护，避免读到更新一半的任务。
    mutable std::mutex mu_;
    ImportTask task_;
    long long seq_ = 0;

    // 从数据库检查是否至少有一个词条，并刷新 ready 标记。
    bool refreshReady();
};
