#pragma once
#include "parity/contracts/error.h"
#include <atomic>
namespace lexica::parity {
class Cancellation {
  std::atomic<bool> flag_{false};

public:
  void request() { flag_.store(true, std::memory_order_release); }
  bool requested() const { return flag_.load(std::memory_order_acquire); }
};
void setCurrentCancellation(const Cancellation *current);
const Cancellation *currentCancellation();
inline void throwIfCancelled(const Cancellation *cancellation) {
  if (cancellation && cancellation->requested())
    throw Error("CANCELLED", "Command cancelled");
}
} // namespace lexica::parity
