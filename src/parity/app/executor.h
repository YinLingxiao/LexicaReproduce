#pragma once
#include "parity/contracts/cancel.h"
#include "parity/contracts/dto.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace lexica::parity {
std::vector<std::string> backgroundCommands();
class Executor {
public:
  using Handler =
      std::function<Json(const Json &, const Cancellation *)>;
  using Reply = std::function<void(Json)>;
  explicit Executor(Handler handler);
  ~Executor();
  Executor(const Executor &) = delete;
  Executor &operator=(const Executor &) = delete;
  void submit(Json request, Reply reply);
  void shutdown();

private:
  struct Job {
    Json request;
    Reply reply;
  };
  void loop(bool background);
  Json cancelled(const Json &request) const;
  Json busy(const Json &request) const;
  Handler handler_;
  std::vector<std::string> background_;
  Cancellation shutdown_;
  Cancellation backgroundStop_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> interactive_;
  std::deque<Job> backgroundQueue_;
  std::vector<std::thread> threads_;
  bool stopped_ = false;
};
} // namespace lexica::parity
