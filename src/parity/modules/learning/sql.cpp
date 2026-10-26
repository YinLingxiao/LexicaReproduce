#include "parity/modules/learning/sql.h"
namespace lexica::parity::learning {
namespace {
void check(sqlite3 *db, int code) {
  if (code != SQLITE_OK)
    throw Error("DB_ERROR", sqlite3_errmsg(db));
}
} // namespace
Stmt::Stmt(sqlite3 *db, const std::string &sql) {
  check(db, sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr));
}
Stmt::~Stmt() { sqlite3_finalize(stmt_); }
Stmt &Stmt::bind(int index, const char *value) {
  return bind(index, std::string(value ? value : ""));
}
Stmt &Stmt::bind(int index, const std::string &value) {
  check(sqlite3_db_handle(stmt_),
        sqlite3_bind_text(stmt_, index, value.c_str(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT));
  return *this;
}
Stmt &Stmt::bind(int index, std::int64_t value) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_int64(stmt_, index, value));
  return *this;
}
Stmt &Stmt::bind(int index, double value) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_double(stmt_, index, value));
  return *this;
}
Stmt &Stmt::null(int index) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_null(stmt_, index));
  return *this;
}
Stmt &Stmt::bind(int index, const std::optional<std::string> &value) {
  if (value)
    bind(index, *value);
  else
    null(index);
  return *this;
}
Stmt &Stmt::bind(int index, const std::optional<std::int64_t> &value) {
  if (value)
    bind(index, *value);
  else
    null(index);
  return *this;
}
bool Stmt::step() {
  const auto code = sqlite3_step(stmt_);
  if (code == SQLITE_ROW)
    return true;
  if (code == SQLITE_DONE)
    return false;
  throw Error("DB_ERROR", sqlite3_errmsg(sqlite3_db_handle(stmt_)));
}
std::int64_t Stmt::i64(int column) const {
  return sqlite3_column_int64(stmt_, column);
}
double Stmt::f64(int column) const { return sqlite3_column_double(stmt_, column); }
std::string Stmt::text(int column) const {
  const auto *value = sqlite3_column_text(stmt_, column);
  return value ? std::string(reinterpret_cast<const char *>(value),
                             sqlite3_column_bytes(stmt_, column))
               : std::string{};
}
bool Stmt::isNull(int column) const {
  return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}
} // namespace lexica::parity::learning
