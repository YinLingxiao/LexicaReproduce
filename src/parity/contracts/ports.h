#pragma once
#include "parity/contracts/clock.h"
#include "parity/contracts/dto.h"
#include "parity/storage/database.h"
#include <functional>
namespace lexica::parity {
// These methods are called inside Database::withConnection. They never open a
// transaction, write encounters, or acquire the Database mutex themselves.
class DictionaryPort {
public:
  virtual ~DictionaryPort() = default;
  virtual std::optional<WordEntry> find(sqlite3 *, const std::string &word) = 0;
  virtual std::optional<WordEntry> byId(sqlite3 *, std::int64_t wordId) = 0;
  virtual std::vector<Suggestion> suggest(sqlite3 *, const std::string &query,
                                          int limit) = 0;
};
// Network and credential stores are injected by F, so tests need no real key.
class KeyStore {
public:
  virtual ~KeyStore() = default;
  virtual std::optional<std::string> get() = 0;
  virtual void set(const std::string &) = 0;
  virtual void remove() = 0;
};
class HttpClient {
public:
  virtual ~HttpClient() = default;
  virtual std::string postJson(const std::string &url,
                               const std::string &bearer,
                               const std::string &body, int timeoutMs) = 0;
};
using EventSink =
    std::function<void(const std::string &event, const Json &payload)>;
struct Context {
  Database &db;
  const Clock &clock;
  DictionaryPort &dictionary;
  EventSink emit;
  // Dispatch copies Context per call; never share mutable request metadata.
  std::string request_id;
};
} // namespace lexica::parity
