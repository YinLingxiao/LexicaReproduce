#pragma once
#include "common/result.h"
#include "common/types.h"
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
    void open(const std::string& dataDir);
    AiConfig config() const;
    ErrorCode save(bool enabled, const std::string& baseUrl, const std::string& model);
    ErrorCode test();
    Result<AiTask> example(EntryId id, const std::string& word);
    Result<AiTask> task(const std::string& id);

private:
    std::string dataDir_;
    AiConfig config_;
    AiTask last_;
    int seq_ = 0;
    void refreshKey();
};
