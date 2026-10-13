// AI 模块：保存配置、请求例句、管理后台任务，并缓存已经生成的例句。
#pragma once
#include "common/result.h"
#include "common/types.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

// AI 配置；keyConfigured 只说明是否配了密钥，不存放密钥本身。
struct AiConfig {
    bool enabled = false;
    std::string baseUrl;
    std::string model;
    bool keyConfigured = false;
};

// AI 生成任务：状态为 pending/running/succeeded/failed，text 保存成功例句。
struct AiTask {
    std::string id;
    std::string status;
    std::string text;
};

// 管理 AI 配置、异步生成任务和例句缓存。
class Ai {
public:
    Ai() = default;
    ~Ai();
    Ai(const Ai&) = delete;
    Ai& operator=(const Ai&) = delete;

    // 设置数据目录，读取已保存的 AI 配置与例句缓存。
    void open(const std::string& dataDir);
    // 返回配置副本，并实时检查环境变量里有没有密钥。
    AiConfig config() const;
    // 检查服务地址格式，更新 AI 配置并保存到文件。
    ErrorCode save(bool enabled, const std::string& baseUrl, const std::string& model);
    // 用当前配置发送简短请求，检查 AI 服务是否能正常响应。
    ErrorCode test();
    // 优先复用任务或缓存；未命中时启动后台线程生成例句，立即返回任务编号。
    Result<AiTask> example(EntryId id, const std::string& word, const std::string& meaning);
    // 根据任务编号读取状态和结果；找不到则返回 AI 错误。
    Result<AiTask> task(const std::string& id);
    // 只读取已缓存的例句，不发网络请求，避免复习选题时等待网络。
    std::string sentence(EntryId id, const std::string& word, const std::string& meaning) const;

private:
    // 内部 AI 任务：用 cacheKey 把相同生成请求关联起来。
    struct Job {
        std::string cacheKey;
        AiTask task;
    };
    // 一条已生成例句，以及判断是否能复用它的缓存标识。
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
    // 后台生成线程和网页请求都会读写任务、缓存，所以访问时需要互斥锁。
    mutable std::mutex mu_;
    // active_ 记录未结束的后台任务；全部结束后通过 idle_ 唤醒析构函数。
    std::condition_variable idle_;
    int active_ = 0;
    long long seq_ = 0;

    // 从本地 JSON 读取 AI 开关、地址和模型；密钥不在这里保存。
    bool loadConfig();
    // 把 AI 开关、地址和模型写入本地配置文件。
    bool saveConfig() const;
    // 读取已缓存例句，忽略字段不完整的记录并限制保留数量。
    bool loadCache();
    // 把当前例句缓存写入本地 JSON 文件，供下次启动继续使用。
    bool saveCache() const;
    // 把词条、释义、服务地址、模型和提示词版本组合成缓存标识。
    std::string cacheKey(EntryId id, const std::string& word, const std::string& meaning) const;
    // 在锁保护下更新后台 AI 任务的状态。
    void setStatus(const std::string& id, const std::string& status);
    // 写回任务结果，成功时更新例句缓存，并减少正在运行的任务数量。
    void finish(const std::string& id, const std::string& key, EntryId entryId, const std::string& word, const std::string& text);
};
