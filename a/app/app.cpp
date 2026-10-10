#include "app/app.h"
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

bool App::ensureDir(const std::string& path) {
    std::error_code ec;
    fs::path dir = fs::u8path(path);
    if (!fs::exists(dir, ec)) {
        if (!fs::create_directories(dir, ec)) return false;
    } else if (!fs::is_directory(dir, ec)) {
        return false;
    }
    fs::path probe = dir / ".__probe";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) return false;
    }
    fs::remove(probe, ec);
    return true;
}

StartupStatus App::start(const std::string& dataDir) {
    dataDir_ = dataDir;
    if (!ensureDir(dataDir_)) {
        status_ = StartupStatus::DirNotWritable;
        return status_;
    }
    status_ = dict_.open(dataDir_);
    if (status_ == StartupStatus::DbCorrupt || status_ == StartupStatus::VersionTooNew) return status_;
    vocab_.open(dataDir_);
    review_.open(dataDir_);
    review_.bind(&dict_, &vocab_);
    ai_.open(dataDir_);
    return status_;
}

void App::shutdown() {
    review_.close();
    vocab_.close();
    dict_.close();
}

StartupStatus App::status() const { return status_; }

bool App::dictReady() const { return dict_.ready(); }

Result<Entry> App::lookup(const std::string& word, const OpId& opId) {
    if (opId.empty()) return Result<Entry>::fail(ErrorCode::BadRequest);
    Result<Entry> found = dict_.lookup(word);
    if (!found.ok()) return found;
    ErrorCode code = vocab_.recordLookup(found.value.id, found.value.word, opId);
    if (code != ErrorCode::Ok) return Result<Entry>::fail(code);
    code = review_.ensureState(found.value.id);
    if (code != ErrorCode::Ok) return Result<Entry>::fail(code);
    return found;
}

Result<std::vector<SuggestItem>> App::suggest(const std::string& prefix) {
    return dict_.suggest(prefix);
}

Result<Entry> App::entry(EntryId id) { return dict_.get(id); }

Result<ImportReport> App::importDict(const std::string& path) {
    Result<ImportReport> report = dict_.importFile(path);
    if (report.ok()) status_ = dict_.ready() ? StartupStatus::Ok : StartupStatus::DictEmpty;
    return report;
}

ErrorCode App::requireEntry(EntryId id, Entry& entry) {
    Result<Entry> found = dict_.get(id);
    if (!found.ok()) return found.code;
    entry = found.value;
    return ErrorCode::Ok;
}

ErrorCode App::setFavorite(EntryId id, bool value) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return code;
    return vocab_.setFavorite(id, entry.word, value);
}

ErrorCode App::setNote(EntryId id, const std::string& text) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return code;
    return vocab_.setNote(id, entry.word, text);
}

ErrorCode App::setPaused(EntryId id, bool value) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return code;
    return vocab_.setPaused(id, entry.word, value);
}

Result<VocabPage> App::vocabList(const VocabQuery& query) {
    return vocab_.list(query, &review_);
}

Result<HistoryPage> App::history(int page, int size) {
    return vocab_.history(page, size);
}

Result<Stats> App::stats() {
    Result<Stats> stats = vocab_.stats();
    if (!stats.ok()) return stats;
    review_.countStages(stats.value.stage0, stats.value.stage1, stats.value.stage2, stats.value.stage3);
    stats.value.paused = vocab_.pausedCount();
    return stats;
}

Result<SessionId> App::reviewStart() { return review_.start(); }

Result<NextQuestion> App::reviewNext(const SessionId& sid) { return review_.next(sid); }

Result<Hint> App::reviewHint(QuestionId qid, int level) { return review_.hint(qid, level); }

Result<Grade> App::reviewSubmit(QuestionId qid, const std::string& answer) {
    Result<Grade> grade = review_.submit(qid, answer);
    if (grade.ok() && !grade.value.duplicate) vocab_.addReview(grade.value.entryId);
    return grade;
}

Result<Summary> App::reviewSummary(const SessionId& sid) { return review_.summary(sid); }

AiConfig App::aiConfig() const { return ai_.config(); }

ErrorCode App::aiSave(bool enabled, const std::string& baseUrl, const std::string& model) {
    return ai_.save(enabled, baseUrl, model);
}

ErrorCode App::aiTest() { return ai_.test(); }

Result<AiTask> App::aiExample(EntryId id, const std::string& word) { return ai_.example(id, word); }

Result<AiTask> App::aiTask(const std::string& id) { return ai_.task(id); }
