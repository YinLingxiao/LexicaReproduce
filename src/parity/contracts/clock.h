#pragma once
#include <chrono>
#include <cstdint>
#include <string>
namespace lexica::parity {
using UtcMillis = std::int64_t;
class Clock {
public:
  virtual ~Clock() = default;
  virtual UtcMillis nowMillis() const = 0;
};
class SystemClock final : public Clock {
public:
  UtcMillis nowMillis() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
};
// Fixed-width RFC3339 UTC milliseconds; matches the upstream SQL sort order.
std::string formatUtc(UtcMillis value);
UtcMillis parseUtc(const std::string &value);
} // namespace lexica::parity
