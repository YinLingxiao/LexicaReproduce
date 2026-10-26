#pragma once
#include "parity/modules/modules.h"
namespace lexica::parity {
class Application {
  SystemClock systemClock_;
  Database database_;
  std::unique_ptr<DictionaryPort> dictionary_;
  Context context_;
  Router router_;

public:
  explicit Application(const std::filesystem::path &dbFile,
                       const Clock *clock = nullptr, EventSink sink = {},
                       bool automaticDictionarySetup = true);
  Json dispatch(const Json &request,
                const Cancellation *cancellation = nullptr);
  void emit(const std::string &event, const Json &payload);
  Json capabilities() const { return router_.capabilities(); }
  Database &database() { return database_; }
};
std::filesystem::path defaultDataDirectory();
} // namespace lexica::parity
