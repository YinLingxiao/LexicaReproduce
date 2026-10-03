#pragma once
#include "common/result.h"
#include "common/types.h"
#include <string>
#include <vector>

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
};

struct SuggestItem {
    EntryId id = 0;
    std::string word;
};

struct ImportReport {
    int added = 0;
    int skipped = 0;
    int failed = 0;
};

class Dict {
public:
    StartupStatus open(const std::string& dataDir);
    void close();
    bool ready() const;
    Result<Entry> lookup(const std::string& word);
    Result<std::vector<SuggestItem>> suggest(const std::string& prefix);
    Result<Entry> get(EntryId id);
    Result<ImportReport> importFile(const std::string& path);

private:
    std::string dataDir_;
    bool ready_ = false;
    std::vector<Entry> entries_;
    const Entry* findWord(const std::string& word) const;
    const Entry* findId(EntryId id) const;
};
