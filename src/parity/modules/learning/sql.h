#pragma once
#include "parity/contracts/error.h"
#include "sqlite3.h"
#include <cstdint>
#include <optional>
#include <string>
namespace lexica::parity::learning {
class Stmt {
  sqlite3_stmt *stmt_ = nullptr;

public:
  Stmt(sqlite3 *db, const std::string &sql);
  ~Stmt();
  Stmt(const Stmt &) = delete;
  Stmt &operator=(const Stmt &) = delete;
  Stmt &bind(int index, const char *value);
  Stmt &bind(int index, const std::string &value);
  Stmt &bind(int index, std::int64_t value);
  Stmt &bind(int index, double value);
  Stmt &null(int index);
  Stmt &bind(int index, const std::optional<std::string> &value);
  Stmt &bind(int index, const std::optional<std::int64_t> &value);
  bool step();
  std::int64_t i64(int column) const;
  double f64(int column) const;
  std::string text(int column) const;
  bool isNull(int column) const;
};
} // namespace lexica::parity::learning
