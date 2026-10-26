#include "parity/app/application.h"
#include <cstdlib>
namespace lexica::parity {
Application::Application(const std::filesystem::path &path, const Clock *clock,
                         EventSink sink)
    : database_(path), dictionary_(makeDictionary()),
      context_{database_,
               clock ? *clock : systemClock_,
               *dictionary_,
               std::move(sink),
               {}} {
  database_.seed();
  registerDictionary(router_);
  registerLearning(router_);
  registerLibrary(router_);
  registerAi(router_);
}
std::filesystem::path defaultDataDirectory() {
  const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
  if (!local || !*local)
    throw Error("DATA_DIRECTORY_ERROR",
                "LOCALAPPDATA is unavailable; specify --data-dir");
  auto defaultDir =
      std::filesystem::path(local) / L"LexicaReproduce" / L"parity-v1";
  // Separate identity protects the original app's DB, key, and WebView profile.
  return defaultDir;
}
} // namespace lexica::parity
