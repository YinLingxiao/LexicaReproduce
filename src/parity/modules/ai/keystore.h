#pragma once
#include "parity/contracts/ports.h"
#include <string>
namespace lexica::parity {
class WindowsCredentialStore final : public KeyStore {
public:
  explicit WindowsCredentialStore(std::wstring target);
  std::optional<std::string> get() override;
  void set(const std::string &key) override;
  void remove() override;
  const std::wstring &target() const { return target_; }

private:
  std::wstring target_;
};
WindowsCredentialStore &productionKeyStore();
} // namespace lexica::parity
