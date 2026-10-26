#include "parity/app/executor.h"
#include "parity/contracts/catalog.h"
#include <algorithm>
namespace lexica::parity {
namespace {
Json envelope(const Json &request, const char *code, const char *message) {
  Json id = nullptr;
  if (request.is_object() && request.contains("request_id"))
    id = request["request_id"];
  return Json{{"protocol_version", 1},
              {"request_id", id},
              {"ok", false},
              {"error", {{"code", code}, {"message", message}}}};
}
} // namespace
std::vector<std::string> backgroundCommands() {
  const auto catalog = Json::parse(kCommands);
  std::vector<std::string> names;
  for (const auto &command : catalog.at("commands"))
    if (command.value("lane", std::string{}) == "background")
      names.push_back(command.at("name").get<std::string>());
  return names;
}
Executor::Executor(Handler handler)
    : handler_(std::move(handler)), background_(backgroundCommands()) {
  for (int i = 0; i < 4; ++i)
    threads_.emplace_back([this] { loop(false); });
  threads_.emplace_back([this] { loop(true); });
}
Executor::~Executor() { shutdown(); }
Json Executor::cancelled(const Json &request) const {
  return envelope(request, "CANCELLED", "Command cancelled");
}
Json Executor::busy(const Json &request) const {
  return envelope(request, "BUSY", "Command queue is full");
}
void Executor::submit(Json request, Reply reply) {
  const auto command = request.is_object() && request.contains("command") &&
                               request["command"].is_string()
                           ? request["command"].get<std::string>()
                           : std::string{};
  const bool background = std::find(background_.begin(), background_.end(),
                                    command) != background_.end();
  Json early;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
      early = cancelled(request);
    else {
      auto &queue = background ? backgroundQueue_ : interactive_;
      const std::size_t limit = background ? 8 : 128;
      if (queue.size() >= limit)
        early = busy(request);
      else
        queue.push_back(Job{std::move(request), std::move(reply)});
    }
  }
  if (!early.is_null())
    reply(std::move(early));
  else
    ready_.notify_all();
}
void Executor::loop(bool background) {
  auto &queue = background ? backgroundQueue_ : interactive_;
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [&] { return stopped_ || !queue.empty(); });
      if (queue.empty())
        return;
      job = std::move(queue.front());
      queue.pop_front();
      if (stopped_) {
        auto answer = cancelled(job.request);
        lock.unlock();
        job.reply(std::move(answer));
        continue;
      }
    }
    Json result;
    try {
      result =
          handler_(job.request, background ? &backgroundStop_ : &shutdown_);
    } catch (const Error &error) {
      result = envelope(job.request, error.code.c_str(), error.what());
    } catch (const std::exception &) {
      result = envelope(job.request, "INTERNAL_ERROR", "Command failed");
    }
    try {
      job.reply(std::move(result));
    } catch (const std::exception &) {
    }
  }
}
void Executor::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
      return;
    stopped_ = true;
  }
  backgroundStop_.request();
  shutdown_.request();
  ready_.notify_all();
  for (auto &thread : threads_)
    if (thread.joinable())
      thread.join();
  threads_.clear();
}
} // namespace lexica::parity
