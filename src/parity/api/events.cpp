#include "parity/api/events.h"
#include <chrono>
namespace lexica::parity {
std::uint64_t EventHub::tip() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sequence_;
}
void EventHub::publish(const std::string &event, const Json &payload) {
  const auto data =
      Json{{"protocol_version", 1}, {"event", event}, {"payload", payload}}
          .dump();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.push_back({++sequence_, "data: " + data + "\n\n"});
    while (items_.size() > 256)
      items_.pop_front();
  }
  ready_.notify_all();
}
std::optional<std::string> EventHub::waitAfter(std::uint64_t &cursor,
                                               int timeoutMs,
                                               const Cancellation *cancellation) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto arrived = ready_.wait_for(
      lock, std::chrono::milliseconds(timeoutMs), [&] {
        return sequence_ > cursor ||
               (cancellation && cancellation->requested());
      });
  if (!arrived || sequence_ <= cursor)
    return std::nullopt;
  std::string out;
  for (const auto &item : items_) {
    if (item.sequence <= cursor)
      continue;
    cursor = item.sequence;
    out += item.data;
  }
  return out;
}
} // namespace lexica::parity
