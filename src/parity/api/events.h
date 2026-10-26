#pragma once
#include "parity/contracts/cancel.h"
#include "parity/contracts/dto.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
namespace lexica::parity {
class EventHub {
  struct Item {
    std::uint64_t sequence = 0;
    std::string data;
  };
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> items_;
  std::uint64_t sequence_ = 0;

public:
  std::uint64_t tip() const;
  void publish(const std::string &event, const Json &payload);
  std::optional<std::string> waitAfter(std::uint64_t &cursor, int timeoutMs,
                                       const Cancellation *cancellation);
};
} // namespace lexica::parity
