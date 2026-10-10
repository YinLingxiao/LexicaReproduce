#pragma once
#include "common/result.h"
#include "common/types.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

enum class StartupStatus {
    Ok,
    DictEmpty,
    DirNotWritable,
    DbCorrupt,
    VersionTooNew
};

const char* startupName(StartupStatus status);

struct Entry {
    EntryId id = 0;
    std::string word;
    std::string phonetic;
    std::string translation;
    std::string definition;
    std::string query;
};

struct SuggestItem {
    EntryId id = 0;
    std::string word;
};

struct ImportReport {
    int added = 0;
    int updated = 0;
    int skipped = 0;
    int failed = 0;
};

struct ImportTask {
    std::string id;
    std::string status;
    ImportReport report;
    ErrorCode error = ErrorCode::Ok;
};

class Dict {
public:
    Dict() = default;
    ~Dict();
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;

    StartupStatus open(const std::string& dataDir);
    void close();
    bool ready() const;
    Result<Entry> lookup(const std::string& word);
    Result<std::vector<SuggestItem>> suggest(const std::string& prefix);
    Result<Entry> get(EntryId id);
    std::vector<std::string> forms(const std::string& word);
    Result<ImportReport> importFile(const std::string& path);
    Result<ImportTask> importStart(const std::string& path);
    Result<ImportTask> importTask(const std::string& id) const;

private:
    std::string dataDir_;
    std::string dbPath_;
    sqlite3* db_ = nullptr;
    std::atomic<bool> ready_{false};
    std::thread worker_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    ImportTask task_;
    long long seq_ = 0;

    bool refreshReady();
};
