#pragma once
#include "common/result.h"
#include "common/types.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

struct AiConfig {
    bool enabled = false;
    std::string baseUrl;
    std::string model;
    bool keyConfigured = false;
};

struct AiTask {
    std::string id;
    std::string status;
    std::string text;
};

class Ai {
public:
    Ai() = default;
    ~Ai();
    Ai(const Ai&) = delete;
    Ai& operator=(const Ai&) = delete;

    void open(const std::string& dataDir);
    AiConfig config() const;
    ErrorCode save(bool enabled, const std::string& baseUrl, const std::string& model);
    ErrorCode test();
    Result<AiTask> example(EntryId id, const std::string& word, const std::string& meaning);
    Result<AiTask> task(const std::string& id);
    std::string sentence(EntryId id, const std::string& word, const std::string& meaning) const;

private:
    struct Job {
        std::string cacheKey;
        AiTask task;
    };
    struct CacheRow {
        std::string key;
        EntryId id = 0;
        std::string word;
        std::string text;
    };

    std::string dataDir_;
    AiConfig config_;
    std::deque<Job> jobs_;
    std::deque<CacheRow> cache_;
    mutable std::mutex mu_;
    std::condition_variable idle_;
    int active_ = 0;
    long long seq_ = 0;

    bool loadConfig();
    bool saveConfig() const;
    bool loadCache();
    bool saveCache() const;
    std::string cacheKey(EntryId id, const std::string& word, const std::string& meaning) const;
    void setStatus(const std::string& id, const std::string& status);
    void finish(const std::string& id, const std::string& key, EntryId entryId, const std::string& word, const std::string& text);
};
