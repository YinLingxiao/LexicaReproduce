#include "parity/app/application.h"
#include "parity/app/paths.h"
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
Json Application::dispatch(const Json &request, const Cancellation *cancellation) {
  auto call = context_;
  call.cancellation = cancellation;
  return router_.dispatch(request, call);
}
void Application::emit(const std::string &event, const Json &payload) {
  if (context_.emit)
    context_.emit(event, payload);
}
std::filesystem::path defaultDataDirectory() {
  return resolveLayout(appRoot()).databaseDirectory;
}
} // namespace lexica::parity
