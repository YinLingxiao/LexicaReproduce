#include "parity/modules/ai/keystore.h"
#include "parity/contracts/error.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
namespace lexica::parity {
namespace {
void credentialError(const char *action) {
  throw Error("RESOURCE_ERROR", std::string("credential store: ") + action +
                                    " failed (" + std::to_string(GetLastError()) +
                                    ")");
}
bool missingCredential(DWORD code) {
  return code == ERROR_NOT_FOUND || code == ERROR_FILE_NOT_FOUND;
}
} // namespace
WindowsCredentialStore::WindowsCredentialStore(std::wstring target)
    : target_(std::move(target)) {}
std::optional<std::string> WindowsCredentialStore::get() {
  PCREDENTIALW cred = nullptr;
  if (!CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
    if (missingCredential(GetLastError()))
      return std::nullopt;
    credentialError("read");
  }
  struct Free {
    PCREDENTIALW cred;
    ~Free() {
      if (cred)
        CredFree(cred);
    }
  } guard{cred};
  if (!cred->CredentialBlob || cred->CredentialBlobSize == 0)
    return std::string{};
  return std::string(reinterpret_cast<const char *>(cred->CredentialBlob),
                     cred->CredentialBlobSize);
}
void WindowsCredentialStore::set(const std::string &key) {
  if (key.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    throw Error("RESOURCE_ERROR", "credential store: key is too long");
  std::wstring user = L"parity-ai";
  CREDENTIALW cred{};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = target_.data();
  cred.CredentialBlobSize = static_cast<DWORD>(key.size());
  cred.CredentialBlob =
      reinterpret_cast<LPBYTE>(const_cast<char *>(key.data()));
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  cred.UserName = user.data();
  if (!CredWriteW(&cred, 0))
    credentialError("write");
}
void WindowsCredentialStore::remove() {
  if (!CredDeleteW(target_.c_str(), CRED_TYPE_GENERIC, 0)) {
    if (missingCredential(GetLastError()))
      return;
    credentialError("delete");
  }
}
WindowsCredentialStore &productionKeyStore() {
  static WindowsCredentialStore store(L"LexicaReproduce/parity-ai-api-key");
  return store;
}
} // namespace lexica::parity
