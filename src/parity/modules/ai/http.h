#pragma once
#include "parity/contracts/ports.h"
namespace lexica::parity {
class WinHttpClient final : public HttpClient {
public:
  explicit WinHttpClient(bool useProxy = true);
  std::string postJson(const std::string &url, const std::string &bearer,
                       const std::string &body, int timeoutMs) override;

private:
  bool useProxy_;
};
} // namespace lexica::parity
