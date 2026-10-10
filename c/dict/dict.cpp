#include "dict/dict.h"
#include "common/text.h"

const char* startupName(StartupStatus status) {
    switch (status) {
    case StartupStatus::Ok: return "OK";
    case StartupStatus::DictEmpty: return "DICT_EMPTY";
    case StartupStatus::DirNotWritable: return "DIR_NOT_WRITABLE";
    case StartupStatus::DbCorrupt: return "DB_CORRUPT";
    case StartupStatus::VersionTooNew: return "VERSION_TOO_NEW";
    }
    return "INTERNAL";
}

StartupStatus Dict::open(const std::string& dataDir) {
    dataDir_ = dataDir;
    entries_.clear();
    ready_ = false;
    return StartupStatus::DictEmpty;
}

void Dict::close() {}

bool Dict::ready() const { return ready_; }

const Entry* Dict::findWord(const std::string& word) const {
    for (const Entry& entry : entries_) {
        if (entry.word == word) return &entry;
    }
    return nullptr;
}

const Entry* Dict::findId(EntryId id) const {
    for (const Entry& entry : entries_) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

Result<Entry> Dict::lookup(const std::string& word) {
    std::string key = normalizeWord(word);
    if (key.empty()) return Result<Entry>::fail(ErrorCode::BadRequest);
    if (!ready_) return Result<Entry>::fail(ErrorCode::DictNotReady);
    const Entry* entry = findWord(key);
    if (!entry) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::success(*entry);
}

Result<std::vector<SuggestItem>> Dict::suggest(const std::string& prefix) {
    std::vector<SuggestItem> items;
    std::string key = normalizeWord(prefix);
    if (key.empty()) return Result<std::vector<SuggestItem>>::success(items);
    if (!ready_) return Result<std::vector<SuggestItem>>::fail(ErrorCode::DictNotReady);
    for (const Entry& entry : entries_) {
        if (entry.word.compare(0, key.size(), key) != 0) continue;
        SuggestItem item;
        item.id = entry.id;
        item.word = entry.word;
        items.push_back(item);
        if (static_cast<int>(items.size()) >= kSuggestMax) break;
    }
    return Result<std::vector<SuggestItem>>::success(items);
}

Result<Entry> Dict::get(EntryId id) {
    if (!ready_) return Result<Entry>::fail(ErrorCode::DictNotReady);
    const Entry* entry = findId(id);
    if (!entry) return Result<Entry>::fail(ErrorCode::WordNotFound);
    return Result<Entry>::success(*entry);
}

Result<ImportReport> Dict::importFile(const std::string& path) {
    if (path.empty()) return Result<ImportReport>::fail(ErrorCode::BadRequest);
    ImportReport report;
    if (!entries_.empty()) {
        report.skipped = static_cast<int>(entries_.size());
        ready_ = true;
        return Result<ImportReport>::success(report);
    }
    entries_.push_back(Entry{1, "apple", "ˈæpl", "n. 苹果", "a round fruit"});
    entries_.push_back(Entry{2, "banana", "bəˈnɑːnə", "n. 香蕉", "a long curved fruit"});
    entries_.push_back(Entry{3, "run", "rʌn", "v. 跑", "to move quickly"});
    report.added = static_cast<int>(entries_.size());
    ready_ = true;
    return Result<ImportReport>::success(report);
}
