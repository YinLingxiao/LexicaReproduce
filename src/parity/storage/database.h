#pragma once
#include "parity/contracts/error.h"
#include "sqlite3.h"
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
namespace lexica::parity {
void execute(sqlite3 *db, const std::string &sql);
class Statement {
  sqlite3_stmt *stmt_ = nullptr;

public:
  Statement(sqlite3 *db, const std::string &sql);
  ~Statement();
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  Statement &bind(int index, const std::string &value);
  Statement &bind(int index, std::int64_t value);
  Statement &null(int index);
  bool step(); // true=row, false=done; all other statuses throw DB_ERROR
  std::int64_t integer(int col) const;
  std::string text(int col) const;
  bool isNull(int col) const;
};
class Transaction {
  sqlite3 *db_;
  bool committed_ = false;

public:
  explicit Transaction(sqlite3 *db);
  ~Transaction();
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  void commit();
};
class Database {
  sqlite3 *db_ = nullptr;
  std::mutex mutex_;
  std::filesystem::path path_;

public:
  explicit Database(const std::filesystem::path &path);
  ~Database();
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  // Callback is serialized. Never keep sqlite handles past it or re-enter it.
  template <class F>
  auto withConnection(F &&f) -> decltype(f(static_cast<sqlite3 *>(nullptr))) {
    std::lock_guard<std::mutex> lock(mutex_);
    return f(db_);
  }
  const std::filesystem::path &path() const { return path_; }
  void seed();
};
} // namespace lexica::parity
