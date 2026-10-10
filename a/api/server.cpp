#include "httplib.h"
#include "json.hpp"
#include "api/server.h"
#include "app/app.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

httplib::Server* gServer = nullptr;
std::mutex gAppMutex;

BOOL WINAPI onCtrl(DWORD) {
    if (gServer) gServer->stop();
    return TRUE;
}

std::string dumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

void sendOk(httplib::Response& res, const json& data) {
    res.set_content(dumpJson(json{{"ok", true}, {"data", data}}), "application/json; charset=utf-8");
}

void sendFail(httplib::Response& res, ErrorCode code, int status = 200) {
    res.status = status;
    json body = {{"ok", false}, {"error", {{"code", errorName(code)}, {"message", errorMessage(code)}}}};
    res.set_content(dumpJson(body), "application/json; charset=utf-8");
}

bool parseBody(const httplib::Request& req, httplib::Response& res, json& out) {
    out = json::parse(req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        sendFail(res, ErrorCode::BadRequest, 400);
        return false;
    }
    return true;
}

bool getString(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

bool getBool(const json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

bool getInt(const json& j, const char* key, std::int64_t& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<std::int64_t>();
    return true;
}

bool parseId(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

int paramInt(const httplib::Request& req, const char* key, int fallback) {
    if (!req.has_param(key)) return fallback;
    std::int64_t value = 0;
    if (!parseId(req.get_param_value(key), value)) return fallback;
    return static_cast<int>(value);
}

int paramFlag(const httplib::Request& req, const char* key) {
    if (!req.has_param(key)) return -1;
    std::string value = req.get_param_value(key);
    if (value == "1") return 1;
    if (value == "0") return 0;
    return -1;
}

json entryJson(const Entry& entry) {
    return {
        {"id", entry.id},
        {"word", entry.word},
        {"phonetic", entry.phonetic},
        {"translation", entry.translation},
        {"definition", entry.definition},
        {"query", entry.query}
    };
}

json importJson(const ImportTask& task) {
    json error = nullptr;
    if (task.error != ErrorCode::Ok) error = {{"code", errorName(task.error)}, {"message", errorMessage(task.error)}};
    return {
        {"id", task.id},
        {"status", task.status},
        {"added", task.report.added},
        {"updated", task.report.updated},
        {"skipped", task.report.skipped},
        {"failed", task.report.failed},
        {"error", error}
    };
}

json taskJson(const AiTask& task) {
    return {{"id", task.id}, {"status", task.status}, {"text", task.text}};
}

std::string contentType(const fs::path& path) {
    std::string ext = path.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

bool hostAllowed(const std::string& host, int port) {
    std::string p = std::to_string(port);
    return host == "127.0.0.1:" + p || host == "localhost:" + p;
}

bool originAllowed(const std::string& origin, int port) {
    std::string p = std::to_string(port);
    return origin == "http://127.0.0.1:" + p || origin == "http://localhost:" + p;
}

void routes(httplib::Server& svr, App& app, int port, const fs::path& webDir) {
    svr.set_pre_routing_handler([port](const httplib::Request& req, httplib::Response& res) {
        if (!hostAllowed(req.get_header_value("Host"), port)) {
            sendFail(res, ErrorCode::Forbidden, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        bool write = req.method != "GET" && req.method != "HEAD";
        std::string origin = req.get_header_value("Origin");
        if (write && !origin.empty() && !originAllowed(origin, port)) {
            sendFail(res, ErrorCode::Forbidden, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/api/status", [&app](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        sendOk(res, {{"state", startupName(app.status())}, {"dict_ready", app.dictReady()}});
    });

    svr.Get("/api/suggest", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<std::vector<SuggestItem>> items = app.suggest(req.get_param_value("q"));
        if (!items.ok()) return sendFail(res, items.code);
        json data = json::array();
        for (const SuggestItem& item : items.value) data.push_back({{"id", item.id}, {"word", item.word}});
        sendOk(res, data);
    });

    svr.Post("/api/lookup", [&app](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::string word;
        std::string op;
        if (!getString(body, "word", word) || !getString(body, "op_id", op)) return sendFail(res, ErrorCode::BadRequest);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Entry> entry = app.lookup(word, op);
        if (!entry.ok()) return sendFail(res, entry.code);
        sendOk(res, entryJson(entry.value));
    });

    svr.Get(R"(/api/entry/(\d+))", [&app](const httplib::Request& req, httplib::Response& res) {
        std::int64_t id = 0;
        if (!parseId(req.matches[1], id)) return sendFail(res, ErrorCode::BadRequest);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Entry> entry = app.entry(id);
        if (!entry.ok()) return sendFail(res, entry.code);
        sendOk(res, entryJson(entry.value));
    });

    svr.Get("/api/history", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<HistoryPage> page = app.history(paramInt(req, "page", 1), paramInt(req, "size", kPageDefault));
        if (!page.ok()) return sendFail(res, page.code);
        json items = json::array();
        for (const HistoryItem& item : page.value.items) {
            Result<Entry> entry = app.entry(item.entryId);
            items.push_back({{"id", item.entryId}, {"word", entry.ok() ? entry.value.word : ""}, {"utc", item.utc}});
        }
        sendOk(res, {{"total", page.value.total}, {"items", items}});
    });

    svr.Get("/api/vocab", [&app](const httplib::Request& req, httplib::Response& res) {
        VocabQuery q;
        q.favorite = paramFlag(req, "fav");
        q.paused = paramFlag(req, "paused");
        q.stage = paramInt(req, "stage", -1);
        q.q = req.get_param_value("q");
        q.page = paramInt(req, "page", 1);
        q.size = paramInt(req, "size", kPageDefault);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<VocabPage> page = app.vocabList(q);
        if (!page.ok()) return sendFail(res, page.code);
        json items = json::array();
        for (const VocabRow& row : page.value.items) {
            items.push_back({
                {"id", row.entryId},
                {"word", row.word},
                {"favorite", row.favorite},
                {"note", row.note},
                {"paused", row.paused},
                {"stage", row.stage}
            });
        }
        sendOk(res, {{"total", page.value.total}, {"items", items}});
    });

    svr.Put(R"(/api/vocab/(\d+)/(favorite|note|paused))", [&app](const httplib::Request& req, httplib::Response& res) {
        std::int64_t id = 0;
        if (!parseId(req.matches[1], id)) return sendFail(res, ErrorCode::BadRequest);
        json body;
        if (!parseBody(req, res, body)) return;
        std::string field = req.matches[2];
        ErrorCode code = ErrorCode::BadRequest;
        std::lock_guard<std::mutex> lock(gAppMutex);
        if (field == "note") {
            std::string text;
            if (!getString(body, "text", text)) return sendFail(res, ErrorCode::BadRequest);
            code = app.setNote(id, text);
        } else {
            bool value = false;
            if (!getBool(body, "value", value)) return sendFail(res, ErrorCode::BadRequest);
            code = field == "favorite" ? app.setFavorite(id, value) : app.setPaused(id, value);
        }
        if (code != ErrorCode::Ok) return sendFail(res, code);
        sendOk(res, json::object());
    });

    svr.Post("/api/review/start", [&app](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<SessionId> session = app.reviewStart();
        if (!session.ok()) return sendFail(res, session.code);
        sendOk(res, {{"session_id", session.value}});
    });

    svr.Get(R"(/api/review/([^/]+)/next)", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<NextQuestion> next = app.reviewNext(req.matches[1]);
        if (!next.ok()) return sendFail(res, next.code);
        if (!next.value.has) return sendOk(res, {{"has", false}});
        const Question& question = next.value.question;
        sendOk(res, {{"has", true}, {"question", {
            {"id", question.id},
            {"session_id", question.sessionId},
            {"entry_id", question.entryId},
            {"prompt", question.prompt},
            {"kind", question.kind},
            {"practice", question.practice}
        }}});
    });

    svr.Post(R"(/api/review/question/(\d+)/hint)", [&app](const httplib::Request& req, httplib::Response& res) {
        std::int64_t id = 0;
        if (!parseId(req.matches[1], id)) return sendFail(res, ErrorCode::BadRequest);
        std::int64_t level = 0;
        if (!req.body.empty()) {
            json body;
            if (!parseBody(req, res, body)) return;
            getInt(body, "level", level);
        }
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Hint> hint = app.reviewHint(id, static_cast<int>(level));
        if (!hint.ok()) return sendFail(res, hint.code);
        sendOk(res, {{"level", hint.value.level}, {"text", hint.value.text}});
    });

    svr.Post(R"(/api/review/question/(\d+)/submit)", [&app](const httplib::Request& req, httplib::Response& res) {
        std::int64_t id = 0;
        if (!parseId(req.matches[1], id)) return sendFail(res, ErrorCode::BadRequest);
        json body;
        if (!parseBody(req, res, body)) return;
        std::string answer;
        if (!getString(body, "answer", answer)) return sendFail(res, ErrorCode::BadRequest);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Grade> grade = app.reviewSubmit(id, answer);
        if (!grade.ok()) return sendFail(res, grade.code);
        sendOk(res, {
            {"entry_id", grade.value.entryId},
            {"correct", grade.value.correct},
            {"duplicate", grade.value.duplicate},
            {"stage", grade.value.stage},
            {"hint_level", grade.value.hintLevel},
            {"practice", grade.value.practice}
        });
    });

    svr.Get(R"(/api/review/([^/]+)/summary)", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Summary> summary = app.reviewSummary(req.matches[1]);
        if (!summary.ok()) return sendFail(res, summary.code);
        sendOk(res, {
            {"formal_count", summary.value.formalCount},
            {"formal_correct", summary.value.formalCorrect},
            {"practice_count", summary.value.practiceCount},
            {"reinforce", summary.value.reinforce}
        });
    });

    svr.Get("/api/stats", [&app](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Stats> stats = app.stats();
        if (!stats.ok()) return sendFail(res, stats.code);
        json days = json::array();
        for (const DayCount& day : stats.value.days) {
            days.push_back({{"date", day.date}, {"lookups", day.lookups}, {"reviews", day.reviews}});
        }
        sendOk(res, {
            {"days", days},
            {"stage0", stats.value.stage0},
            {"stage1", stats.value.stage1},
            {"stage2", stats.value.stage2},
            {"stage3", stats.value.stage3},
            {"paused", stats.value.paused}
        });
    });

    svr.Post("/api/import", [&app](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::string path;
        if (!getString(body, "path", path)) return sendFail(res, ErrorCode::BadRequest);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<ImportTask> task = app.importStart(path);
        if (!task.ok()) return sendFail(res, task.code);
        sendOk(res, importJson(task.value));
    });

    svr.Get(R"(/api/import/([^/]+))", [&app](const httplib::Request& req, httplib::Response& res) {
        Result<ImportTask> task = app.importTask(req.matches[1]);
        if (!task.ok()) return sendFail(res, task.code);
        sendOk(res, importJson(task.value));
    });

    svr.Get("/api/ai/config", [&app](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        AiConfig config = app.aiConfig();
        sendOk(res, {
            {"enabled", config.enabled},
            {"base_url", config.baseUrl},
            {"model", config.model},
            {"key_configured", config.keyConfigured}
        });
    });

    svr.Put("/api/ai/config", [&app](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        bool enabled = false;
        if (!getBool(body, "enabled", enabled)) return sendFail(res, ErrorCode::BadRequest);
        std::string baseUrl;
        std::string model;
        getString(body, "base_url", baseUrl);
        getString(body, "model", model);
        std::lock_guard<std::mutex> lock(gAppMutex);
        ErrorCode code = app.aiSave(enabled, baseUrl, model);
        if (code != ErrorCode::Ok) return sendFail(res, code);
        sendOk(res, json::object());
    });

    svr.Post("/api/ai/test", [&app](const httplib::Request&, httplib::Response& res) {
        ErrorCode code = app.aiTest();
        if (code != ErrorCode::Ok) return sendFail(res, code);
        sendOk(res, json::object());
    });

    svr.Post("/api/ai/example", [&app](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::int64_t id = 0;
        if (!getInt(body, "id", id)) return sendFail(res, ErrorCode::BadRequest);
        std::string word;
        getString(body, "word", word);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<AiTask> task = app.aiExample(id, word);
        if (!task.ok()) return sendFail(res, task.code);
        sendOk(res, taskJson(task.value));
    });

    svr.Get(R"(/api/ai/task/([^/]+))", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<AiTask> task = app.aiTask(req.matches[1]);
        if (!task.ok()) return sendFail(res, task.code);
        sendOk(res, taskJson(task.value));
    });

    svr.Get(R"(/(.*))", [webDir](const httplib::Request& req, httplib::Response& res) {
        std::string rel = req.matches[1];
        if (rel.rfind("api/", 0) == 0) return sendFail(res, ErrorCode::BadRequest, 404);
        if (rel.empty()) rel = "index.html";
        if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) {
            return sendFail(res, ErrorCode::BadRequest, 404);
        }
        fs::path file = webDir / fs::u8path(rel);
        std::ifstream in(file, std::ios::binary);
        if (!in) return sendFail(res, ErrorCode::BadRequest, 404);
        std::ostringstream ss;
        ss << in.rdbuf();
        res.set_content(ss.str(), contentType(file));
    });

    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (!res.body.empty()) return;
        sendFail(res, ErrorCode::BadRequest, res.status);
    });
}

}

int runServer(App& app, int port, const std::string& webDir) {
    httplib::Server svr;
    svr.set_read_timeout(5, 0);
    svr.set_write_timeout(5, 0);
    svr.set_payload_max_length(1024 * 1024);
    routes(svr, app, port, fs::u8path(webDir));
    gServer = &svr;
    SetConsoleCtrlHandler(onCtrl, TRUE);
    bool ok = svr.listen("127.0.0.1", port);
    SetConsoleCtrlHandler(onCtrl, FALSE);
    gServer = nullptr;
    if (!ok) {
        std::cerr << "INTERNAL\n";
        return 1;
    }
    return 0;
}
