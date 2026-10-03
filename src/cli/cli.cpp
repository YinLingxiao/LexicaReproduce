#include "app/app.h"
#include "cli/cli.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void show(ErrorCode code) {
    std::cout << errorName(code) << " " << errorMessage(code) << "\n";
}

std::string ask(const char* label) {
    std::cout << label;
    std::string line;
    if (!std::getline(std::cin, line)) return "";
    return line;
}

bool askId(EntryId& out) {
    std::string text = ask("id: ");
    if (text.empty()) return false;
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (!end || *end != '\0' || value <= 0) return false;
    out = static_cast<EntryId>(value);
    return true;
}

void showEntry(const Entry& entry) {
    std::cout << entry.id << " " << entry.word << " " << entry.phonetic << " " << entry.translation << "\n";
}

}

int runCli(App& app) {
    SessionId sid;
    QuestionId qid = 0;
    int seq = 1;
    std::string choice;
    while (true) {
        std::cout << "\n1导入 2查词 3建议 4收藏 5笔记 6历史 7词汇 8开始复习 9下一题 10提示 11提交 12摘要 13统计 0退出\n";
        if (!std::getline(std::cin, choice)) break;
        if (choice == "0") break;
        else if (choice == "1") {
            Result<ImportReport> report = app.importDict(ask("路径: "));
            if (!report.ok()) show(report.code);
            else std::cout << report.value.added << " " << report.value.skipped << " " << report.value.failed << "\n";
        } else if (choice == "2") {
            Result<Entry> entry = app.lookup(ask("单词: "), "cli-" + std::to_string(seq++));
            if (!entry.ok()) show(entry.code);
            else showEntry(entry.value);
        } else if (choice == "3") {
            Result<std::vector<SuggestItem>> items = app.suggest(ask("前缀: "));
            if (!items.ok()) show(items.code);
            else for (const SuggestItem& item : items.value) std::cout << item.id << " " << item.word << "\n";
        } else if (choice == "4") {
            EntryId id = 0;
            if (!askId(id)) { show(ErrorCode::BadRequest); continue; }
            bool on = ask("1收藏 0取消: ") == "1";
            ErrorCode code = app.setFavorite(id, on);
            if (code != ErrorCode::Ok) show(code);
        } else if (choice == "5") {
            EntryId id = 0;
            if (!askId(id)) { show(ErrorCode::BadRequest); continue; }
            ErrorCode code = app.setNote(id, ask("笔记: "));
            if (code != ErrorCode::Ok) show(code);
        } else if (choice == "6") {
            Result<HistoryPage> page = app.history(1, kPageDefault);
            if (!page.ok()) show(page.code);
            else for (const HistoryItem& item : page.value.items) std::cout << item.entryId << " " << item.utc << "\n";
        } else if (choice == "7") {
            Result<VocabPage> page = app.vocabList(VocabQuery{});
            if (!page.ok()) show(page.code);
            else for (const VocabRow& row : page.value.items) std::cout << row.entryId << " " << row.word << " " << row.favorite << "\n";
        } else if (choice == "8") {
            Result<SessionId> session = app.reviewStart();
            if (!session.ok()) show(session.code);
            else { sid = session.value; std::cout << sid << "\n"; }
        } else if (choice == "9") {
            Result<NextQuestion> next = app.reviewNext(sid);
            if (!next.ok()) show(next.code);
            else if (!next.value.has) std::cout << "empty\n";
            else {
                qid = next.value.question.id;
                std::cout << qid << " " << next.value.question.prompt << "\n";
            }
        } else if (choice == "10") {
            Result<Hint> hint = app.reviewHint(qid, 0);
            if (!hint.ok()) show(hint.code);
            else std::cout << hint.value.level << " " << hint.value.text << "\n";
        } else if (choice == "11") {
            Result<Grade> grade = app.reviewSubmit(qid, ask("答案: "));
            if (!grade.ok()) show(grade.code);
            else std::cout << grade.value.correct << " " << grade.value.duplicate << "\n";
        } else if (choice == "12") {
            Result<Summary> summary = app.reviewSummary(sid);
            if (!summary.ok()) show(summary.code);
            else std::cout << summary.value.formalCount << " " << summary.value.formalCorrect << "\n";
        } else if (choice == "13") {
            Result<Stats> stats = app.stats();
            if (!stats.ok()) show(stats.code);
            else std::cout << "stage " << stats.value.stage0 << " " << stats.value.stage1 << " " << stats.value.stage2 << " " << stats.value.stage3 << "\n";
        } else {
            show(ErrorCode::BadRequest);
        }
    }
    return 0;
}
