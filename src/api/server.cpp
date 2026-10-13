// 网页接口：接收 HTTP 请求，检查参数，调用 App，再把结果转成 JSON。
#include "httplib.h"
#include "json.hpp"
#include "api/server.h"
#include "app/app.h"
#include <cerrno>
#include <climits>
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
// App 各模块共用这把锁，同一时间只处理一个受保护的业务操作。
std::mutex gAppMutex;

// 收到控制台退出信号时，通知 HTTP 服务停止监听。
BOOL WINAPI onCtrl(DWORD) {
    if (gServer) gServer->stop();
    return TRUE;
}

// 把 JSON 转为响应文本；遇到非法 UTF-8 时用替代字符输出。
std::string dumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// 统一成功响应格式：ok=true，实际结果放在 data 中。
void sendOk(httplib::Response& res, const json& data) {
    res.set_content(dumpJson(json{{"ok", true}, {"data", data}}), "application/json; charset=utf-8");
}

// 统一失败响应格式：ok=false，附错误码和中文说明。
void sendFail(httplib::Response& res, ErrorCode code, int status = 200) {
    res.status = status;
    json body = {{"ok", false}, {"error", {{"code", errorName(code)}, {"message", errorMessage(code)}}}};
    res.set_content(dumpJson(body), "application/json; charset=utf-8");
}

// 解析请求体，要求它是 JSON 对象；格式错误时返回 HTTP 400。
bool parseBody(const httplib::Request& req, httplib::Response& res, json& out) {
    out = json::parse(req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        sendFail(res, ErrorCode::BadRequest, 400);
        return false;
    }
    return true;
}

// 只有字段存在且为字符串时才读取，避免把错误类型当作合法参数。
bool getString(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

// 检查并读取 JSON 中的布尔字段。
bool getBool(const json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

// 检查并读取 JSON 中的整数编号或提示级别。
bool getInt(const json& j, const char* key, std::int64_t& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<std::int64_t>();
    return true;
}

// 把路径或参数中的数字文本转成整数，拒绝夹杂非数字的内容。
bool parseId(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

// 读取 URL 查询参数中的整数；缺失或格式不对时用默认值，超出 int 范围则拒绝。
bool paramInt(const httplib::Request& req, const char* key, int fallback, int& out) {
    out = fallback;
    if (!req.has_param(key)) return true;
    const std::string text = req.get_param_value(key);
    if (text.empty()) return true;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (!end || end == text.c_str() || *end != '\0') return true;
    if (errno == ERANGE || value < INT_MIN || value > INT_MAX) return false;
    out = static_cast<int>(value);
    return true;
}

// 读取 1/0 筛选开关；没有合法开关时返回 -1，表示不筛选。
int paramFlag(const httplib::Request& req, const char* key) {
    if (!req.has_param(key)) return -1;
    std::string value = req.get_param_value(key);
    if (value == "1") return 1;
    if (value == "0") return 0;
    return -1;
}

// 把内部词条转成前端使用的字段格式。
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

// 把导入任务转成 JSON，包含状态、计数和失败原因。
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

// 把 AI 任务转成 JSON，返回编号、状态和生成文本。
json taskJson(const AiTask& task) {
    return {{"id", task.id}, {"status", task.status}, {"text", task.text}};
}

// 根据文件后缀设置响应类型，让浏览器正确识别网页、脚本和图片。
std::string contentType(const fs::path& path) {
    std::string ext = path.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

// 检查请求的 Host 是否为本机服务及当前端口。
bool hostAllowed(const std::string& host, int port) {
    std::string p = std::to_string(port);
    return host == "127.0.0.1:" + p || host == "localhost:" + p;
}

// 检查写请求的来源页面是否属于本机服务。
bool originAllowed(const std::string& origin, int port) {
    std::string p = std::to_string(port);
    return origin == "http://127.0.0.1:" + p || origin == "http://localhost:" + p;
}

// 登记全部接口和静态文件处理；每个请求在这里转交给相应功能。
void routes(httplib::Server& svr, App& app, int port, const fs::path& webDir) {
    // 所有请求先检查主机地址；写入请求还检查页面来源。
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

    // 服务状态：是否正常启动、是否已有词库。
    svr.Get("/api/status", [&app](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        sendOk(res, {{"state", startupName(app.status())}, {"dict_ready", app.dictReady()}});
    });

    // 输入联想：只返回候选，不写查词历史。
    svr.Get("/api/suggest", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<std::vector<SuggestItem>> items = app.suggest(req.get_param_value("q"));
        if (!items.ok()) return sendFail(res, items.code);
        json data = json::array();
        for (const SuggestItem& item : items.value) data.push_back({{"id", item.id}, {"word", item.word}});
        sendOk(res, data);
    });

    // 正式查词：读取 word 和 op_id，返回词条并记录学习事件。
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

    // 按编号读词条，用于详情展示，不记一次查词。
    svr.Get(R"(/api/entry/(\d+))", [&app](const httplib::Request& req, httplib::Response& res) {
        std::int64_t id = 0;
        if (!parseId(req.matches[1], id)) return sendFail(res, ErrorCode::BadRequest);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<Entry> entry = app.entry(id);
        if (!entry.ok()) return sendFail(res, entry.code);
        sendOk(res, entryJson(entry.value));
    });

    // 查词历史：分页读取事件，再补上对应的单词文本。
    svr.Get("/api/history", [&app](const httplib::Request& req, httplib::Response& res) {
        int pageNo = 1;
        int size = kPageDefault;
        if (!paramInt(req, "page", 1, pageNo) || !paramInt(req, "size", kPageDefault, size)) return sendFail(res, ErrorCode::BadRequest);
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<HistoryPage> page = app.history(pageNo, size);
        if (!page.ok()) return sendFail(res, page.code);
        json items = json::array();
        for (const HistoryItem& item : page.value.items) {
            Result<Entry> entry = app.entry(item.entryId);
            items.push_back({{"id", item.entryId}, {"word", entry.ok() ? entry.value.word : ""}, {"utc", item.utc}});
        }
        sendOk(res, {{"total", page.value.total}, {"items", items}});
    });

    // 个人词汇：解析筛选与分页条件，返回收藏、笔记和阶段。
    svr.Get("/api/vocab", [&app](const httplib::Request& req, httplib::Response& res) {
        VocabQuery q;
        q.favorite = paramFlag(req, "fav");
        q.paused = paramFlag(req, "paused");
        q.q = req.get_param_value("q");
        if (!paramInt(req, "stage", -1, q.stage) || !paramInt(req, "page", 1, q.page) || !paramInt(req, "size", kPageDefault, q.size)) {
            return sendFail(res, ErrorCode::BadRequest);
        }
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

    // 共用更新入口：根据路径末尾字段修改收藏、笔记或暂停。
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

    // 创建一轮复习，返回 session_id 给后续取题使用。
    svr.Post("/api/review/start", [&app](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<SessionId> session = app.reviewStart();
        if (!session.ok()) return sendFail(res, session.code);
        sendOk(res, {{"session_id", session.value}});
    });

    // 读取下一题；has=false 表示当前轮没有可作答题目。
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

    // 取得提示；未指定 level 时自动取下一级。
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

    // 提交答案，返回掌握情况、是否重复及最新阶段。
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

    // 读取本轮总结，正式题和重练分别统计。
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

    // 读取近 14 天活动，以及各学习阶段和暂停词数。
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

    // 创建后台导入任务，立即返回编号与当前状态。
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

    // 轮询导入进度；不需要等待全局业务锁。
    svr.Get(R"(/api/import/([^/]+))", [&app](const httplib::Request& req, httplib::Response& res) {
        Result<ImportTask> task = app.importTask(req.matches[1]);
        if (!task.ok()) return sendFail(res, task.code);
        sendOk(res, importJson(task.value));
    });

    // 读取 AI 配置；前端只知道有没有密钥，不会拿到密钥。
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

    // 保存 AI 开关、地址和模型。
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

    // 测试 AI 连接；网络等待期间不持有全局业务锁。
    svr.Post("/api/ai/test", [&app](const httplib::Request&, httplib::Response& res) {
        ErrorCode code = app.aiTest();
        if (code != ErrorCode::Ok) return sendFail(res, code);
        sendOk(res, json::object());
    });

    // 请求生成例句，返回可供轮询的任务。
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

    // 轮询 AI 例句任务状态和文本。
    svr.Get(R"(/api/ai/task/([^/]+))", [&app](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gAppMutex);
        Result<AiTask> task = app.aiTask(req.matches[1]);
        if (!task.ok()) return sendFail(res, task.code);
        sendOk(res, taskJson(task.value));
    });

    // 静态文件入口：根路径显示 index.html，其他路径读取网页资源。
    svr.Get(R"(/(.*))", [webDir](const httplib::Request& req, httplib::Response& res) {
        std::string rel = req.matches[1];
        if (rel.rfind("api/", 0) == 0) return sendFail(res, ErrorCode::BadRequest, 404);
        if (rel.empty()) rel = "index.html";
        // 拒绝上级目录、反斜杠和盘符，避免请求读到网页目录之外的文件。
        if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) {
            return sendFail(res, ErrorCode::BadRequest, 404);
        }
        fs::path file = webDir / fs::u8path(rel);
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            const auto dot = rel.find_last_of('.');
            const auto slash = rel.find_last_of('/');
            const bool asset = dot != std::string::npos && (slash == std::string::npos || dot > slash);
            if (asset) return sendFail(res, ErrorCode::BadRequest, 404);
            file = webDir / "index.html";
            in.clear();
            in.open(file, std::ios::binary);
            if (!in) return sendFail(res, ErrorCode::BadRequest, 404);
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        res.set_content(ss.str(), contentType(file));
    });

    // 为没有响应正文的 HTTP 错误补上统一 JSON 错误内容。
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (!res.body.empty()) return;
        sendFail(res, ErrorCode::BadRequest, res.status);
    });
}

}

void requestServerStop() {
    if (gServer) gServer->stop();
}

// 设置超时与请求大小限制，只在 127.0.0.1 上启动服务。
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
