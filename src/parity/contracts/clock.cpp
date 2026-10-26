#include "parity/contracts/clock.h"
#include "parity/contracts/error.h"
#include <ctime>
#include <iomanip>
#include <sstream>
namespace lexica::parity {
std::string formatUtc(UtcMillis value) {
  auto sec = value / 1000;
  int ms = static_cast<int>(value % 1000);
  if (ms < 0) {
    ms += 1000;
    --sec;
  }
  std::time_t t = static_cast<std::time_t>(sec);
  std::tm tm{};
  if (gmtime_s(&tm, &t))
    throw Error("BAD_REQUEST", "Timestamp is out of range");
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S.") << std::setw(3)
      << std::setfill('0') << ms << 'Z';
  return out.str();
}
UtcMillis parseUtc(const std::string &value) {
  if (value.size() != 24 || value[19] != '.' || value[23] != 'Z')
    throw Error("BAD_REQUEST", "Expected UTC RFC3339 milliseconds");
  std::tm tm{};
  std::istringstream input(value.substr(0, 19));
  input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (input.fail() || value[20] < '0' || value[20] > '9' || value[21] < '0' ||
      value[21] > '9' || value[22] < '0' || value[22] > '9')
    throw Error("BAD_REQUEST", "Invalid timestamp");
  const auto seconds = _mkgmtime64(&tm);
  const auto ms =
      (value[20] - '0') * 100 + (value[21] - '0') * 10 + value[22] - '0';
  const auto result = seconds * 1000 + ms;
  if (seconds < 0 || formatUtc(result) != value)
    throw Error("BAD_REQUEST", "Invalid calendar date");
  return result;
}
} // namespace lexica::parity
