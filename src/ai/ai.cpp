#include "ai/ai.h"
#include <cstdlib>

void Ai::refreshKey() {
    const char* key = std::getenv("LEXICA_AI_KEY");
    config_.keyConfigured = key && key[0] != '\0';
}

void Ai::open(const std::string& dataDir) {
    dataDir_ = dataDir;
    refreshKey();
}

AiConfig Ai::config() const {
    AiConfig config = config_;
    config.keyConfigured = config_.keyConfigured;
    return config;
}

ErrorCode Ai::save(bool enabled, const std::string& baseUrl, const std::string& model) {
    config_.enabled = enabled;
    config_.baseUrl = baseUrl;
    config_.model = model;
    refreshKey();
    return ErrorCode::Ok;
}

ErrorCode Ai::test() {
    refreshKey();
    if (!config_.enabled) return ErrorCode::AiDisabled;
    if (!config_.keyConfigured) return ErrorCode::AiKeyMissing;
    return ErrorCode::Ok;
}

Result<AiTask> Ai::example(EntryId id, const std::string& word) {
    (void)id;
    (void)word;
    refreshKey();
    last_ = AiTask{};
    last_.id = "t" + std::to_string(++seq_);
    last_.status = "failed";
    if (!config_.enabled) return Result<AiTask>::fail(ErrorCode::AiDisabled);
    if (!config_.keyConfigured) return Result<AiTask>::fail(ErrorCode::AiKeyMissing);
    return Result<AiTask>::fail(ErrorCode::AiInvalid);
}

Result<AiTask> Ai::task(const std::string& id) {
    if (id.empty() || id != last_.id) return Result<AiTask>::fail(ErrorCode::AiInvalid);
    return Result<AiTask>::success(last_);
}
