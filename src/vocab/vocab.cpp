#include "vocab/vocab.h"
#include "common/clock.h"
#include "common/text.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {

int utf8Chars(const std::string& text) {
    int count = 0;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

std::string utcDate(std::int64_t utc) {
    std::time_t t = static_cast<std::time_t>(utc);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

int clampPage(int page) { return page < 1 ? 1 : page; }

int clampSize(int size) {
    if (size <= 0) return kPageDefault;
    if (size > kPageMax) return kPageMax;
    return size;
}

}

void Vocab::open(const std::string& dataDir) { dataDir_ = dataDir; }

void Vocab::close() {}

Vocab::Row* Vocab::find(EntryId id) {
    for (Row& row : rows_) {
        if (row.entryId == id) return &row;
    }
    return nullptr;
}

const Vocab::Row* Vocab::find(EntryId id) const {
    for (const Row& row : rows_) {
        if (row.entryId == id) return &row;
    }
    return nullptr;
}

Vocab::Row& Vocab::ensure(EntryId id, const std::string& word) {
    if (Row* row = find(id)) {
        if (!word.empty()) row->word = word;
        return *row;
    }
    Row row;
    row.entryId = id;
    row.word = word;
    rows_.push_back(row);
    return rows_.back();
}

ErrorCode Vocab::recordLookup(EntryId id, const std::string& word, const OpId& opId) {
    if (opId.empty()) return ErrorCode::BadRequest;
    for (const OpId& seen : ops_) {
        if (seen == opId) return ErrorCode::Ok;
    }
    ops_.push_back(opId);
    ensure(id, word);
    HistoryItem item;
    item.entryId = id;
    item.utc = systemClock().nowUtc();
    events_.push_back(item);
    return ErrorCode::Ok;
}

ErrorCode Vocab::setFavorite(EntryId id, const std::string& word, bool value) {
    ensure(id, word).favorite = value;
    return ErrorCode::Ok;
}

ErrorCode Vocab::setNote(EntryId id, const std::string& word, const std::string& text) {
    if (utf8Chars(text) > kNoteMaxChars) return ErrorCode::BadRequest;
    ensure(id, word).note = text;
    return ErrorCode::Ok;
}

ErrorCode Vocab::setPaused(EntryId id, const std::string& word, bool value) {
    ensure(id, word).paused = value;
    return ErrorCode::Ok;
}

Result<VocabPage> Vocab::list(const VocabQuery& query, const StageSource* stages) {
    std::string needle = normalizeWord(query.q);
    std::vector<Row> matched;
    for (const Row& row : rows_) {
        if (query.favorite == 1 && !row.favorite) continue;
        if (query.favorite == 0 && row.favorite) continue;
        if (query.paused == 1 && !row.paused) continue;
        if (query.paused == 0 && row.paused) continue;
        int stage = stages ? stages->stageOf(row.entryId) : 0;
        if (query.stage >= 0 && stage != query.stage) continue;
        if (!needle.empty() && row.word.find(needle) == std::string::npos) continue;
        matched.push_back(row);
    }
    std::sort(matched.begin(), matched.end(), [](const Row& a, const Row& b) {
        return a.entryId < b.entryId;
    });
    VocabPage page;
    page.total = static_cast<int>(matched.size());
    int size = clampSize(query.size);
    int index = (clampPage(query.page) - 1) * size;
    for (int i = index; i < page.total && i < index + size; ++i) {
        const Row& row = matched[static_cast<std::size_t>(i)];
        VocabRow item;
        item.entryId = row.entryId;
        item.word = row.word;
        item.favorite = row.favorite;
        item.note = row.note;
        item.paused = row.paused;
        item.stage = stages ? stages->stageOf(row.entryId) : 0;
        page.items.push_back(item);
    }
    return Result<VocabPage>::success(page);
}

Result<HistoryPage> Vocab::history(int pageNo, int sizeIn) {
    HistoryPage page;
    page.total = static_cast<int>(events_.size());
    int size = clampSize(sizeIn);
    int skip = (clampPage(pageNo) - 1) * size;
    int seen = 0;
    for (int i = page.total - 1; i >= 0; --i) {
        if (seen++ < skip) continue;
        if (static_cast<int>(page.items.size()) >= size) break;
        page.items.push_back(events_[static_cast<std::size_t>(i)]);
    }
    return Result<HistoryPage>::success(page);
}

Result<Stats> Vocab::stats() {
    Stats stats;
    std::int64_t now = systemClock().nowUtc();
    for (int i = 13; i >= 0; --i) {
        DayCount day;
        day.date = utcDate(now - static_cast<std::int64_t>(i) * 86400);
        for (const HistoryItem& item : events_) {
            if (utcDate(item.utc) == day.date) ++day.lookups;
        }
        for (const HistoryItem& item : reviews_) {
            if (utcDate(item.utc) == day.date) ++day.reviews;
        }
        stats.days.push_back(day);
    }
    stats.paused = pausedCount();
    return Result<Stats>::success(stats);
}

ErrorCode Vocab::addReview(EntryId id) {
    HistoryItem item;
    item.entryId = id;
    item.utc = systemClock().nowUtc();
    reviews_.push_back(item);
    return ErrorCode::Ok;
}

bool Vocab::isPaused(EntryId id) const {
    const Row* row = find(id);
    return row && row->paused;
}

int Vocab::pausedCount() const {
    int count = 0;
    for (const Row& row : rows_) {
        if (row.paused) ++count;
    }
    return count;
}
