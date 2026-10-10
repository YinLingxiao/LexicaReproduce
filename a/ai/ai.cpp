#include "ai/ai.h"
#include "common/text.h"
#include "json.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const int kTimeoutMs = 15000;
const std::size_t kResponseMax = 16384;
const std::size_t kSentenceMax = 500;
const std::size_t kJobKeep = 100;
const std::size_t kCacheKeep = 500;
const char* kPromptVersion = "v2";

std::string trim(const std::string& in) {
    std::size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) ++begin;
    std::size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) --end;
    return in.substr(begin, end - begin);
}

std::string envKey() {
    const char* key = std::getenv("LEXICA_AI_KEY");
    return key ? key : "";
}

std::wstring utf16(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return std::to_string(hash);
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string endpoint(std::string base) {
    base = trim(base);
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string suffix = "/chat/completions";
    if (base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) return base;
    return base + suffix;
}

struct Url {
    bool https = false;
    std::string host;
    INTERNET_PORT port = 0;
    std::string path;
};

bool parseUrl(const std::string& input, Url& url) {
    std::string rest = input;
    if (rest.rfind("https://", 0) == 0) {
        url.https = true;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        url.https = false;
        rest = rest.substr(7);
    } else {
        return false;
    }
    std::size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    url.path = slash == std::string::npos ? "/" : rest.substr(slash);
    url.port = url.https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    std::string portText;
    if (!hostport.empty() && hostport[0] == '[') {
        std::size_t close = hostport.find(']');
        if (close == std::string::npos) return false;
        url.host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size()) {
            if (hostport[close + 1] != ':') return false;
            portText = hostport.substr(close + 2);
        }
    } else {
        std::size_t colon = hostport.find(':');
        url.host = hostport.substr(0, colon);
        if (colon != std::string::npos) portText = hostport.substr(colon + 1);
    }
    if (url.host.empty() || url.host.find('@') != std::string::npos) return false;
    if (!portText.empty()) {
        for (char c : portText) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        if (portText.size() > 5) return false;
        int port = std::stoi(portText);
        if (port <= 0 || port > 65535) return false;
        url.port = static_cast<INTERNET_PORT>(port);
    }
    return true;
}

bool localHost(const std::string& host) {
    std::string lower = normalizeWord(host);
    return lower == "127.0.0.1" || lower == "localhost" || lower == "::1";
}

bool allowedUrl(const std::string& baseUrl, Url& url) {
    if (!parseUrl(endpoint(baseUrl), url)) return false;
    return url.https || localHost(url.host);
}

std::string messageText(const json& body) {
    if (!body.is_object() || !body.contains("choices") || !body["choices"].is_array() || body["choices"].empty()) return "";
    const json& choice = body["choices"][0];
    if (!choice.is_object() || !choice.contains("message") || !choice["message"].is_object()) return "";
    const json& message = choice["message"];
    if (!message.contains("content")) return "";
    const json& content = message["content"];
    if (content.is_string()) return trim(content.get<std::string>());
    if (!content.is_array()) return "";
    std::string text;
    for (const json& part : content) {
        if (part.is_string()) text += part.get<std::string>();
        else if (part.is_object() && part.contains("text") && part["text"].is_string()) text += part["text"].get<std::string>();
    }
    return trim(text);
}

bool validSentence(const std::string& text, const std::string& word) {
    if (text.empty() || text.size() > kSentenceMax || text.find(' ') == std::string::npos) return false;
    if (!containsWholeWord(text, word)) return false;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80) {
            unsigned char d = static_cast<unsigned char>(text[i + 2]);
            if (d == 0x93 || d == 0x94 || d == 0x98 || d == 0x99 || d == 0x9C || d == 0x9D) {
                i += 3;
                continue;
            }
        }
        return false;
    }
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        char c = text[i];
        if ((c != '.' && c != '!' && c != '?') || text[i + 1] != ' ') continue;
        std::size_t j = i + 1;
        while (j < text.size() && text[j] == ' ') ++j;
        if (j < text.size()) return false;
    }
    return true;
}

struct Watchdog {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool fired = false;
    HINTERNET request = nullptr;
};

ErrorCode failure(DWORD err) {
    return err == ERROR_WINHTTP_TIMEOUT ? ErrorCode::AiTimeout : ErrorCode::AiInvalid;
}

ErrorCode transfer(HINTERNET request, const std::wstring& headers, std::string& body, DWORD& status, std::string& response) {
    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<LPVOID>(&body[0]),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
        return failure(GetLastError());
    }
    if (!WinHttpReceiveResponse(request, nullptr)) return failure(GetLastError());
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) return failure(GetLastError());
        if (avail == 0) return ErrorCode::Ok;
        if (response.size() + avail > kResponseMax) return ErrorCode::AiInvalid;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, &chunk[0], avail, &read)) return failure(GetLastError());
        chunk.resize(read);
        response += chunk;
    }
}

ErrorCode chat(const std::string& baseUrl, const std::string& model, const std::string& key, const std::string& prompt, std::string& out) {
    out.clear();
    Url url;
    if (!allowedUrl(baseUrl, url)) return ErrorCode::AiInvalid;
    std::string body = json{{"model", model}, {"messages", json::array({json{{"role", "user"}, {"content", prompt}}})}}.dump();
    HINTERNET session = WinHttpOpen(L"lexica", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return ErrorCode::AiInvalid;
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    HINTERNET connect = WinHttpConnect(session, utf16(url.host).c_str(), url.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", utf16(url.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (!request) {
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ErrorCode::AiInvalid;
    }
    auto dog = std::make_shared<Watchdog>();
    dog->request = request;
    std::thread([dog] {
        std::unique_lock<std::mutex> lock(dog->mu);
        if (dog->cv.wait_for(lock, std::chrono::milliseconds(kTimeoutMs), [&dog] { return dog->done; })) return;
        dog->fired = true;
        WinHttpCloseHandle(dog->request);
    }).detach();
    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + utf16(key) + L"\r\n";
    DWORD status = 0;
    std::string response;
    ErrorCode code = transfer(request, headers, body, status, response);
    bool fired = false;
    {
        std::lock_guard<std::mutex> lock(dog->mu);
        dog->done = true;
        fired = dog->fired;
    }
    dog->cv.notify_all();
    if (!fired) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (fired) return ErrorCode::AiTimeout;
    if (code != ErrorCode::Ok) return code;
    if (status != 200) return ErrorCode::AiInvalid;
    json parsed = json::parse(response, nullptr, false);
    if (parsed.is_discarded()) return ErrorCode::AiInvalid;
    out = messageText(parsed);
    if (out.empty() || (key.size() >= 8 && out.find(key) != std::string::npos)) {
        out.clear();
        return ErrorCode::AiInvalid;
    }
    return ErrorCode::Ok;
}

bool writeFile(const fs::path& path, const std::string& body) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << body;
        if (!out) return false;
    }
    return MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}

Ai::~Ai() {
    std::unique_lock<std::mutex> lock(mu_);
    idle_.wait(lock, [this] { return active_ == 0; });
}

void Ai::open(const std::string& dataDir) {
    std::lock_guard<std::mutex> lock(mu_);
    dataDir_ = dataDir;
    seq_ = nowMs();
    loadConfig();
    loadCache();
}

bool Ai::loadConfig() {
    config_ = AiConfig{};
    json parsed = json::parse(readFile(fs::u8path(dataDir_) / "ai-config.json"), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return false;
    if (parsed.contains("enabled") && parsed["enabled"].is_boolean()) config_.enabled = parsed["enabled"].get<bool>();
    if (parsed.contains("base_url") && parsed["base_url"].is_string()) config_.baseUrl = parsed["base_url"].get<std::string>();
    if (parsed.contains("model") && parsed["model"].is_string()) config_.model = parsed["model"].get<std::string>();
    return true;
}

bool Ai::saveConfig() const {
    json body = {{"enabled", config_.enabled}, {"base_url", config_.baseUrl}, {"model", config_.model}};
    return writeFile(fs::u8path(dataDir_) / "ai-config.json", body.dump());
}

bool Ai::loadCache() {
    cache_.clear();
    json parsed = json::parse(readFile(fs::u8path(dataDir_) / "ai-cache.json"), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return false;
    for (const json& row : parsed) {
        if (!row.is_object() || !row.contains("key") || !row["key"].is_string() || !row.contains("text") || !row["text"].is_string() ||
            !row.contains("word") || !row["word"].is_string() || !row.contains("entry_id") || !row["entry_id"].is_number_integer()) {
            continue;
        }
        CacheRow item;
        item.key = row["key"].get<std::string>();
        item.text = row["text"].get<std::string>();
        item.word = row["word"].get<std::string>();
        item.id = row["entry_id"].get<EntryId>();
        cache_.push_back(item);
    }
    while (cache_.size() > kCacheKeep) cache_.pop_front();
    return true;
}

bool Ai::saveCache() const {
    json rows = json::array();
    for (const CacheRow& row : cache_) rows.push_back({{"key", row.key}, {"entry_id", row.id}, {"word", row.word}, {"text", row.text}});
    return writeFile(fs::u8path(dataDir_) / "ai-cache.json", rows.dump());
}

std::string Ai::cacheKey(EntryId id, const std::string& word, const std::string& meaning) const {
    return std::to_string(id) + "|" + word + "|" + fnv1a(meaning) + "|" + config_.baseUrl + "|" + config_.model + "|" + kPromptVersion;
}

AiConfig Ai::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    AiConfig config = config_;
    config.keyConfigured = !envKey().empty();
    return config;
}

ErrorCode Ai::save(bool enabled, const std::string& baseUrl, const std::string& model) {
    std::string url = trim(baseUrl);
    Url parsed;
    if (!url.empty() && !allowedUrl(url, parsed)) return ErrorCode::BadRequest;
    std::lock_guard<std::mutex> lock(mu_);
    config_.enabled = enabled;
    config_.baseUrl = url;
    config_.model = trim(model);
    return saveConfig() ? ErrorCode::Ok : ErrorCode::DbError;
}

ErrorCode Ai::test() {
    std::string baseUrl, model, key = envKey();
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!config_.enabled) return ErrorCode::AiDisabled;
        if (key.empty()) return ErrorCode::AiKeyMissing;
        if (config_.baseUrl.empty() || config_.model.empty()) return ErrorCode::AiInvalid;
        baseUrl = config_.baseUrl;
        model = config_.model;
    }
    std::string text;
    return chat(baseUrl, model, key, "Reply with OK.", text);
}

std::string Ai::sentence(EntryId id, const std::string& word, const std::string& meaning) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!config_.enabled) return "";
    std::string key = cacheKey(id, normalizeWord(word), meaning);
    for (const CacheRow& row : cache_) {
        if (row.key == key) return row.text;
    }
    return "";
}

void Ai::setStatus(const std::string& id, const std::string& status) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Job& job : jobs_) {
        if (job.task.id == id) job.task.status = status;
    }
}

void Ai::finish(const std::string& id, const std::string& key, EntryId entryId, const std::string& word, const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Job& job : jobs_) {
        if (job.task.id != id) continue;
        job.task.status = text.empty() ? "failed" : "succeeded";
        job.task.text = text;
    }
    if (!text.empty()) {
        bool found = false;
        for (CacheRow& row : cache_) {
            if (row.key != key) continue;
            row.text = text;
            found = true;
        }
        if (!found) cache_.push_back(CacheRow{key, entryId, word, text});
        while (cache_.size() > kCacheKeep) cache_.pop_front();
        saveCache();
    }
    --active_;
    idle_.notify_all();
}

Result<AiTask> Ai::example(EntryId id, const std::string& word, const std::string& meaning) {
    std::string target = normalizeWord(word);
    if (target.empty()) return Result<AiTask>::fail(ErrorCode::BadRequest);
    std::string secret = envKey();
    std::lock_guard<std::mutex> lock(mu_);
    if (!config_.enabled) return Result<AiTask>::fail(ErrorCode::AiDisabled);
    if (secret.empty()) return Result<AiTask>::fail(ErrorCode::AiKeyMissing);
    Url url;
    if (config_.baseUrl.empty() || config_.model.empty() || !allowedUrl(config_.baseUrl, url)) return Result<AiTask>::fail(ErrorCode::AiInvalid);
    std::string key = cacheKey(id, target, meaning);
    for (const Job& job : jobs_) {
        if (job.cacheKey == key && (job.task.status == "pending" || job.task.status == "running")) return Result<AiTask>::success(job.task);
    }
    while (jobs_.size() >= kJobKeep && jobs_.front().task.status != "pending" && jobs_.front().task.status != "running") jobs_.pop_front();
    Job job;
    job.cacheKey = key;
    job.task.id = "t" + std::to_string(++seq_);
    for (const CacheRow& row : cache_) {
        if (row.key != key) continue;
        job.task.status = "succeeded";
        job.task.text = row.text;
        jobs_.push_back(job);
        return Result<AiTask>::success(job.task);
    }
    job.task.status = "pending";
    jobs_.push_back(job);
    ++active_;
    std::string baseUrl = config_.baseUrl;
    std::string model = config_.model;
    std::string taskId = job.task.id;
    std::thread([this, taskId, key, id, target, meaning, baseUrl, model, secret] {
        setStatus(taskId, "running");
        std::string prompt = "Write one short English sentence that uses the word \"" + target + "\" exactly in this form";
        if (!meaning.empty()) prompt += ", with this meaning: " + meaning;
        prompt += ". Return only the sentence, on one line, without quotes.";
        std::string text;
        ErrorCode code = ErrorCode::AiInvalid;
        try {
            code = chat(baseUrl, model, secret, prompt, text);
        } catch (...) {
            code = ErrorCode::AiInvalid;
        }
        if (code != ErrorCode::Ok || !validSentence(text, target)) text.clear();
        finish(taskId, key, id, target, text);
    }).detach();
    return Result<AiTask>::success(job.task);
}

Result<AiTask> Ai::task(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Job& job : jobs_) {
        if (job.task.id == id) return Result<AiTask>::success(job.task);
    }
    return Result<AiTask>::fail(ErrorCode::AiInvalid);
}
