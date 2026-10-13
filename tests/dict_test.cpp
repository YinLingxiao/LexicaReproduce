// 词库模块验收测试；通过 CMake 构建，再用 CTest 或直接运行 dict_test 执行。
#include "dict/dict.h"
#include "sqlite3.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static int gFail = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "PASS  " : "FAIL  ") << name << "\n";
    if (!ok) ++gFail;
}

sqlite3* openDb(const fs::path& dir) {
    sqlite3* db = nullptr;
    const std::string path = (dir / "lexica.db").u8string();
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

int scalar(const fs::path& dir, const char* sql) {
    sqlite3* db = openDb(dir);
    sqlite3_stmt* st = nullptr;
    int n = -1;
    if (db && sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    return n;
}

void writeFile(const fs::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

std::string csvHeader() { return "word,phonetic,definition,translation,exchange\n"; }

std::string badCsv() {
    std::string body = csvHeader();
    body += "apple,a,CHANGED,tr,s:apples\n";
    for (int i = 1; i < 5000; ++i) body += "w" + std::to_string(i) + ",p,d,t,\n";
    body += "\"broken\n";
    return body;
}

std::string largeCsv() {
    std::string body = csvHeader();
    body += "apple,a,updated,tr,s:apples\n";
    for (int i = 1; i <= 5001; ++i) body += "n" + std::to_string(i) + ",p,d,t,\n";
    return body;
}

ImportTask waitTask(Dict& dict, const std::string& id) {
    for (int i = 0; i < 200; ++i) {
        Result<ImportTask> task = dict.importTask(id);
        if (task.ok() && task.value.status != "running") return task.value;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ImportTask failed;
    failed.status = "running";
    return failed;
}

int main() {
    const fs::path dir = test_support::freshDirectory("dict", "import");
    const fs::path base = dir / "base.csv";
    const fs::path bad = dir / "bad.csv";
    const fs::path large = dir / "large.csv";
    const fs::path finalFile = dir / "final.csv";
    const fs::path caseFile = dir / "case.csv";
    writeFile(base, csvHeader() + "apple,a,original,tr,\nbanana,b,yellow,tr,s:bananas\n");
    writeFile(bad, badCsv());
    writeFile(large, largeCsv());
    writeFile(finalFile, csvHeader() + "apple,a,final,tr,s:apples\n");

    Dict dict;
    check(dict.open(dir.u8string()) == StartupStatus::DictEmpty, "open empty dictionary");
    Result<ImportReport> baseReport = dict.importFile(base.u8string());
    check(baseReport.ok() && baseReport.value.added == 2, "base import adds two words");
    Result<Entry> apple = dict.lookup("apple");
    Result<Entry> banana = dict.lookup("banana");
    check(apple.ok() && banana.ok(), "base words are searchable");
    const EntryId appleId = apple.ok() ? apple.value.id : 0;
    const EntryId bananaId = banana.ok() ? banana.value.id : 0;
    check(apple.ok() && apple.value.definition == "original", "base apple definition");
    check(dict.forms("banana").size() == 1 && dict.forms("banana")[0] == "bananas", "base banana form");
    check(dict.forms("apple").empty(), "apple has no form yet");
    const int baseCount = scalar(dir, "SELECT COUNT(*) FROM dictionary_entries");

    Result<ImportReport> badSync = dict.importFile(bad.u8string());
    check(!badSync.ok() && badSync.code == ErrorCode::BadRequest, "broken csv import fails");
    Result<Entry> appleAfter = dict.lookup("apple");
    check(appleAfter.ok() && appleAfter.value.id == appleId && appleAfter.value.definition == "original", "failed import keeps apple id and definition");
    check(dict.lookup("banana").value.id == bananaId, "failed import keeps banana id");
    check(dict.lookup("w1").code == ErrorCode::WordNotFound, "failed import does not keep staged new words");
    check(dict.forms("apple").empty(), "failed import does not add apple form");
    check(dict.forms("banana").size() == 1, "failed import keeps the old form");
    check(scalar(dir, "SELECT COUNT(*) FROM dictionary_entries") == baseCount, "failed import does not change entry count");
    check(scalar(dir, "SELECT COUNT(*) FROM dictionary_forms WHERE form='apples'") == 0, "apples form was not committed");

    Result<ImportReport> largeReport = dict.importFile(large.u8string());
    check(largeReport.ok() && largeReport.value.added == 5001 && largeReport.value.updated == 1, "large import applies every staged batch");
    Result<Entry> appleLarge = dict.lookup("apple");
    check(appleLarge.ok() && appleLarge.value.id == appleId && appleLarge.value.definition == "updated", "successful reimport keeps apple id");
    check(dict.lookup("banana").value.id == bananaId, "word absent from the file keeps its id");
    check(dict.lookup("n1").ok() && dict.lookup("n5001").ok(), "rows before and after the 5000-row boundary are both stored");
    check(dict.forms("apple").size() == 1 && dict.forms("apple")[0] == "apples", "successful import adds the new form");
    check(dict.forms("banana").size() == 1, "old form remains after a later import");
    const EntryId n1 = dict.lookup("n1").value.id;
    Result<ImportReport> again = dict.importFile(large.u8string());
    check(again.ok() && dict.lookup("apple").value.id == appleId && dict.lookup("n1").value.id == n1 && dict.lookup("banana").value.id == bananaId,
          "importing the same file again keeps existing ids");

    Result<ImportTask> backgroundBad = dict.importStart(bad.u8string());
    check(backgroundBad.ok(), "background import starts");
    ImportTask badTask = waitTask(dict, backgroundBad.value.id);
    check(badTask.status == "failed", "background import of a broken file fails");
    check(dict.lookup("apple").value.id == appleId && dict.lookup("apple").value.definition == "updated", "background failure keeps the previous dictionary");
    check(scalar(dir, "SELECT COUNT(*) FROM dictionary_entries WHERE word='w1'") == 0, "background failure does not keep partial rows");

    Result<ImportTask> backgroundOk = dict.importStart(finalFile.u8string());
    ImportTask okTask = waitTask(dict, backgroundOk.value.id);
    check(backgroundOk.ok() && okTask.status == "succeeded", "background import can still succeed");
    check(dict.lookup("apple").value.id == appleId && dict.lookup("apple").value.definition == "final", "background success keeps the id and applies the change");
    dict.close();

    const fs::path caseDir = test_support::freshDirectory("dict", "case");
    writeFile(caseDir / "a.csv", csvHeader() + "apple,a,defA,tr,\n");
    writeFile(caseDir / "b.csv", csvHeader() + "Apple,a,defB,tr,\n");
    writeFile(caseDir / "c.csv", csvHeader() + "apple,a,defC,tr,\n");
    Dict cased;
    cased.open(caseDir.u8string());
    check(cased.importFile((caseDir / "a.csv").u8string()).ok(), "import lowercase apple");
    const EntryId caseId = cased.lookup("apple").value.id;
    check(cased.importFile((caseDir / "b.csv").u8string()).ok(), "uppercase duplicate is accepted as a skipped row");
    check(cased.lookup("apple").value.id == caseId && cased.lookup("apple").value.definition == "defA", "existing lowercase spelling wins");
    check(cased.importFile((caseDir / "c.csv").u8string()).ok(), "same spelling can update");
    check(cased.lookup("apple").value.id == caseId && cased.lookup("apple").value.definition == "defC", "update keeps the original id");
    cased.close();
    test_support::removeDirectory(caseDir);
    test_support::removeDirectory(dir);

    std::cout << (gFail == 0 ? "ALL DICT TESTS PASSED\n" : "DICT TEST FAILURES\n");
    return gFail == 0 ? 0 : 1;
}
