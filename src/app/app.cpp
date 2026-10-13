// 应用编排模块：把查词、个人词汇、复习和 AI 串起来，供网页和控制台调用。
#include "app/app.h"
#include "common/text.h"
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

// 创建数据目录，并写入一个临时文件，确认目录确实可以保存数据。
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

// 依次打开词库、个人词汇、复习和 AI，并接好复习所需的回调。
StartupStatus App::start(const std::string& dataDir) {
    dataDir_ = dataDir;
    if (!ensureDir(dataDir_)) {
        status_ = StartupStatus::DirNotWritable;
        return status_;
    }
    // 词库模块先创建或升级共享数据库，个人词汇模块和 复习模块才能打开自己的数据表。
    status_ = dict_.open(dataDir_);
    if (status_ != StartupStatus::Ok && status_ != StartupStatus::DictEmpty) return status_;
    vocab_.open(dataDir_);
    if (!review_.open(dataDir_)) {
        status_ = StartupStatus::DbCorrupt;
        return status_;
    }
    review_.bind(&dict_, &vocab_);
    ai_.open(dataDir_);
    // 把“获取 AI 缓存例句”的能力交给复习模块；回调为空串时复习模块自动出本地题。
    review_.setSentenceSource([this](EntryId id, const std::string& word) {
        Entry entry;
        if (requireEntry(id, entry) != ErrorCode::Ok || entry.word != word) return std::string();
        return ai_.sentence(id, entry.word, meaningOf(entry));
    });
    return status_;
}

// 关闭复习、词汇和词库连接，释放数据库资源。
void App::shutdown() {
    review_.close();
    vocab_.close();
    dict_.close();
}

// 返回启动状态；词库导入后，空库状态会随之变成正常。
StartupStatus App::status() const {
    if (status_ != StartupStatus::Ok && status_ != StartupStatus::DictEmpty) return status_;
    return dict_.ready() ? StartupStatus::Ok : StartupStatus::DictEmpty;
}

// 查询词库中是否已经有可用词条。
bool App::dictReady() const { return dict_.ready(); }

// 正式查词：先查词库模块，命中后让个人词汇模块记历史，再让复习模块初始化学习状态。
Result<Entry> App::lookup(const std::string& word, const OpId& opId) {
    if (opId.empty()) return Result<Entry>::fail(ErrorCode::BadRequest);
    Result<Entry> found = dict_.lookup(word);
    if (!found.ok()) return found;
    // 只有正式查词命中才记录事件；候选提示和只读词条不走这里。
    ErrorCode code = vocab_.recordLookup(found.value.id, found.value.word, opId);
    if (code != ErrorCode::Ok) return Result<Entry>::fail(code);
    code = review_.ensureState(found.value.id);
    if (code != ErrorCode::Ok) return Result<Entry>::fail(code);
    return found;
}

// 按输入前缀取最多 8 个候选词；这一步不算正式查词。
Result<std::vector<SuggestItem>> App::suggest(const std::string& prefix) {
    return dict_.suggest(prefix);
}

// 按编号读取词条，只读数据，不记历史。
Result<Entry> App::entry(EntryId id) { return dict_.get(id); }

// 同步导入词库，调用者要等导入结束后才能取得结果。
Result<ImportReport> App::importDict(const std::string& path) { return dict_.importFile(path); }

// 启动后台导入并立即返回任务；已有导入在运行时返回当前任务。
Result<ImportTask> App::importStart(const std::string& path) { return dict_.importStart(path); }

// 按任务编号查询导入状态和累计处理数量。
Result<ImportTask> App::importTask(const std::string& id) const { return dict_.importTask(id); }

// 优先使用英文释义；没有英文释义时改用中文翻译。
std::string App::meaningOf(const Entry& entry) {
    return entry.definition.empty() ? entry.translation : entry.definition;
}

// 先确认词条存在；成功时把完整词条放进输出参数 entry。
ErrorCode App::requireEntry(EntryId id, Entry& entry) {
    Result<Entry> found = dict_.get(id);
    if (!found.ok()) return found.code;
    entry = found.value;
    return ErrorCode::Ok;
}

// 设置或取消收藏；同一个词共用一条个人词汇记录。
ErrorCode App::setFavorite(EntryId id, bool value) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return code;
    return vocab_.setFavorite(id, entry.word, value);
}

// 保存笔记；超过 5000 个字符时拒绝，不覆盖原笔记。
ErrorCode App::setNote(EntryId id, const std::string& text) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return code;
    return vocab_.setNote(id, entry.word, text);
}

// 设置是否暂停复习；暂停后选题时会跳过这个词。
ErrorCode App::setPaused(EntryId id, bool value) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return code;
    return vocab_.setPaused(id, entry.word, value);
}

// 把筛选条件交给个人词汇模块，并让个人词汇模块从 复习模块读取每个词的阶段。
Result<VocabPage> App::vocabList(const VocabQuery& query) {
    return vocab_.list(query, &review_);
}

// 读取正式查词历史，按最近发生的记录优先排列并分页。
Result<HistoryPage> App::history(int page, int size) {
    return vocab_.history(page, size);
}

// 先取个人词汇模块的活动统计，再补上复习模块的阶段词数和暂停数量。
Result<Stats> App::stats() {
    Result<Stats> stats = vocab_.stats();
    if (!stats.ok()) return stats;
    review_.countStages(stats.value.stage0, stats.value.stage1, stats.value.stage2, stats.value.stage3);
    stats.value.paused = vocab_.pausedCount();
    return stats;
}

// 让复习模块按到期时间创建一轮复习，返回这轮的编号。
Result<SessionId> App::reviewStart() { return review_.start(); }

// 取得这一轮尚未作答的下一道题。
Result<NextQuestion> App::reviewNext(const SessionId& sid) { return review_.next(sid); }

// 请求指定题目的提示；level=0 表示取下一级。
Result<Hint> App::reviewHint(QuestionId qid, int level) { return review_.hint(qid, level); }

// 交给复习模块判分；正式题补记复习次数，同一题只记一次，重练不计数。
Result<Grade> App::reviewSubmit(QuestionId qid, const std::string& answer) {
    Result<Grade> grade = review_.submit(qid, answer);
    if (!grade.ok() || grade.value.practice) return grade;
    const ErrorCode recorded = vocab_.addReview(grade.value.entryId, qid);
    if (recorded != ErrorCode::Ok) return Result<Grade>::fail(recorded);
    return grade;
}

// 读取这一轮的正式成绩、重练次数和待巩固单词。
Result<Summary> App::reviewSummary(const SessionId& sid) { return review_.summary(sid); }

// 读取 AI 配置，只返回是否配了密钥，不返回密钥内容。
AiConfig App::aiConfig() const { return ai_.config(); }

// 保存 AI 开关、服务地址和模型名称。
ErrorCode App::aiSave(bool enabled, const std::string& baseUrl, const std::string& model) {
    return ai_.save(enabled, baseUrl, model);
}

// 发起一次简短 AI 请求，用来检查连接和配置。
ErrorCode App::aiTest() { return ai_.test(); }

// 先检查词条与单词是否对应，再请求生成或读取缓存例句。
Result<AiTask> App::aiExample(EntryId id, const std::string& word) {
    Entry entry;
    ErrorCode code = requireEntry(id, entry);
    if (code != ErrorCode::Ok) return Result<AiTask>::fail(code);
    if (!word.empty() && normalizeWord(word) != entry.word) return Result<AiTask>::fail(ErrorCode::BadRequest);
    return ai_.example(id, entry.word, meaningOf(entry));
}

// 查询后台 AI 任务的状态和例句结果。
Result<AiTask> App::aiTask(const std::string& id) { return ai_.task(id); }
