#pragma once
#include <stdexcept>
#include <string>
namespace lexica::parity {
class Error : public std::runtime_error {
public:
  std::string code;
  Error(std::string c, std::string message)
      : std::runtime_error(std::move(message)), code(std::move(c)) {}
};
[[noreturn]] inline void notImplemented(const std::string &command) {
  throw Error("NOT_IMPLEMENTED",
              command + " is scheduled for the next development phase.");
}
} // namespace lexica::parity
