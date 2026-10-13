// 在真正的导入 SQL 执行点暂停后台线程，验证暂存期间主库仍可写；不向业务接口加入测试钩子。
#include "app/app.h"
#include "sqlite3.h"
#include "test_support.h"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
int failures = 0;

void check(bool ok, const char* message) {
    std::cout << (ok ? "PASS  " : "FAIL  ") << message << '\n';
    if (!ok) ++failures;
}

struct StagingGate {
    std::mutex mutex;
    std::condition_variable changed;
    int reached = 0;
    int released = 0;
    bool cancelled = false;
    bool timedOut = false;

    void pause(int phase) {
        std::unique_lock<std::mutex> lock(mutex);
        if (cancelled || reached >= phase) return;
        reached = phase;
        changed.notify_all();
        if (!changed.wait_for(lock, 15s, [&] { return cancelled || released >= phase; })) {
            timedOut = true;
            cancelled = true;
        }
    }

    bool wait(int phase) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 10s, [&] { return reached >= phase; });
    }

    void release(int phase, bool cancel = false) {
        std::lock_guard<std::mutex> lock(mutex);
        released = phase;
        cancelled = cancelled || cancel;
        changed.notify_all();
    }
};

StagingGate* activeGate = nullptr;

// SQLite 的自动扩展只为新连接安装语句跟踪。两个暂停点分别在词条和词形暂存事务内部。
int traceStatement(unsigned kind, void*, void* statement, void*) {
    if (kind != SQLITE_TRACE_STMT || !activeGate) return 0;
    const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
    if (!sql) return 0;
    const std::string text(sql);
    if (text.rfind("INSERT INTO stage_entry", 0) == 0) activeGate->pause(1);
    if (text.rfind("INSERT INTO stage_form", 0) == 0) activeGate->pause(2);
    return 0;
}

int installTrace(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_trace_v2(db, SQLITE_TRACE_STMT, traceStatement, nullptr);
}

struct WriteResult {
    bool lookup;
    bool favorite;
    bool note;
};
}

int main() {
    const auto dir = test_support::freshDirectory("import-concurrency", "main");
    App app;
    check(app.start(dir.u8string()) == StartupStatus::DictEmpty, "start empty app");
    check(app.importDict(test_support::dataFile("test_words.csv").u8string()).ok(), "import baseline dictionary");
    const auto entry = app.entry(1);
    if (!entry.ok()) return 1;
    const auto source = dir / "import.csv";
    {
        std::ofstream out(source, std::ios::binary);
        out << "word,phonetic,definition,translation,exchange\n"
               "apple,,updated meaning,updated translation,s:apples\n";
        for (int i = 0; i < 6000; ++i) out << "new" << i << ",,a definition,translation,\n";
    }

    StagingGate gate;
    activeGate = &gate;
    const auto extension = reinterpret_cast<void(*)()>(installTrace);
    check(sqlite3_auto_extension(extension) == SQLITE_OK, "install staging observer");
    const auto task = app.importStart(source.u8string());
    check(task.ok(), "start background import");
    if (task.ok()) {
        for (int phase = 1; phase <= 2; ++phase) {
            const bool reached = gate.wait(phase);
            check(reached, phase == 1 ? "paused inside entry staging" : "paused inside form staging");
            if (!reached) { gate.release(phase, true); break; }
            const auto live = app.entry(entry.value.id);
            check(live.ok() && live.value.definition == entry.value.definition, "staged content is not visible in live dictionary");
            auto writes = std::async(std::launch::async, [&app, &entry, phase] {
                return WriteResult{
                    app.lookup("apple", "during-import-" + std::to_string(phase)).ok(),
                    app.setFavorite(entry.value.id, true) == ErrorCode::Ok,
                    app.setNote(entry.value.id, "note " + std::to_string(phase)) == ErrorCode::Ok
                };
            });
            const bool ready = writes.wait_for(2s) == std::future_status::ready;
            check(ready, "lookup, favorite and note finish while importer is still paused");
            gate.release(phase, !ready);
            const auto result = writes.get();
            check(result.lookup && result.favorite && result.note, "concurrent app writes succeed");
            if (!ready) break;
        }
        ImportTask completed;
        for (int i = 0; i < 400; ++i) {
            const auto current = app.importTask(task.value.id);
            if (current.ok()) completed = current.value;
            if (completed.status == "succeeded" || completed.status == "failed") break;
            std::this_thread::sleep_for(25ms);
        }
        check(completed.status == "succeeded", "import commits successfully after concurrent writes");
        check(completed.report.added == 6000 && completed.report.updated == 1, "import counts retain staged changes");
        const auto updated = app.entry(entry.value.id);
        check(updated.ok() && updated.value.id == entry.value.id && updated.value.definition == "updated meaning", "atomic apply preserves entry id and updates definition");
        const auto vocab = app.vocabList(VocabQuery{});
        check(vocab.ok() && vocab.value.total == 1 && vocab.value.items[0].favorite && vocab.value.items[0].note == "note 2", "concurrent personal data survives final apply");
        const auto history = app.history(1, 20);
        check(history.ok() && history.value.total == 2, "both concurrent lookup events are retained");
    }
    gate.release(2, true);
    app.shutdown();
    sqlite3_cancel_auto_extension(extension);
    activeGate = nullptr;
    check(!gate.timedOut, "staging observer did not time out");
    test_support::removeDirectory(dir);
    return failures == 0 ? 0 : 1;
}
