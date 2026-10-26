#include "parity/modules/ai/http.h"
#include "parity/contracts/error.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
namespace lexica::parity {
namespace {
struct Internet {
  HINTERNET handle = nullptr;
  ~Internet() {
    if (handle)
      WinHttpCloseHandle(handle);
  }
};
std::wstring toWide(const std::string &value) {
  if (value.empty())
    return {};
  auto n = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                               static_cast<int>(value.size()), nullptr, 0);
  if (n <= 0)
    throw Error("NETWORK_ERROR", "AI request could not be encoded");
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), n);
  return out;
}
std::string redact(std::string message, const std::string &secret) {
  if (secret.empty())
    return message;
  for (std::size_t at = 0; (at = message.find(secret, at)) != std::string::npos;) {
    message.replace(at, secret.size(), "[redacted]");
    at += 10;
  }
  return message;
}
[[noreturn]] void transportError(DWORD code, const std::string &bearer) {
  std::string text = "AI request failed: " + std::to_string(code);
  if (code == ERROR_WINHTTP_TIMEOUT)
    text = "AI request timed out";
  else if (code == ERROR_WINHTTP_CANNOT_CONNECT)
    text = "AI request failed: cannot connect";
  else if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED)
    text = "AI request failed: name not resolved";
  else if (code == ERROR_WINHTTP_CONNECTION_ERROR)
    text = "AI request failed: connection error";
  throw Error("NETWORK_ERROR", redact(text, bearer));
}
std::string firstLine(const std::string &body) {
  auto end = body.find_first_of("\r\n");
  auto line = body.substr(0, end == std::string::npos ? body.size() : end);
  std::size_t chars = 0;
  std::size_t bytes = 0;
  while (bytes < line.size() && chars < 200) {
    auto c = static_cast<unsigned char>(line[bytes]);
    std::size_t n = 1;
    if ((c & 0x80) == 0)
      n = 1;
    else if ((c & 0xE0) == 0xC0)
      n = 2;
    else if ((c & 0xF0) == 0xE0)
      n = 3;
    else if ((c & 0xF8) == 0xF0)
      n = 4;
    if (bytes + n > line.size())
      break;
    bytes += n;
    ++chars;
  }
  return line.substr(0, bytes);
}
} // namespace
WinHttpClient::WinHttpClient(bool useProxy) : useProxy_(useProxy) {}
std::string WinHttpClient::postJson(const std::string &url,
                                    const std::string &bearer,
                                    const std::string &body, int timeoutMs) {
  if (bearer.find('\r') != std::string::npos ||
      bearer.find('\n') != std::string::npos)
    throw Error("NETWORK_ERROR", "API key is not usable");
  if (timeoutMs <= 0)
    timeoutMs = 15000;
  auto wideUrl = toWide(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  std::wstring host(512, L'\0');
  std::wstring path(4096, L'\0');
  std::wstring extra(2048, L'\0');
  parts.lpszHostName = host.data();
  parts.dwHostNameLength = static_cast<DWORD>(host.size());
  parts.lpszUrlPath = path.data();
  parts.dwUrlPathLength = static_cast<DWORD>(path.size());
  parts.lpszExtraInfo = extra.data();
  parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
  if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0,
                       &parts))
    throw Error("NETWORK_ERROR", "AI service address is invalid");
  host.resize(parts.dwHostNameLength);
  path.resize(parts.dwUrlPathLength);
  extra.resize(parts.dwExtraInfoLength);
  if (path.empty())
    path = L"/";
  path += extra;
  Internet session{WinHttpOpen(
      L"LexicaReproduce/1.0",
      useProxy_ ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                : WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.handle)
    transportError(GetLastError(), bearer);
  auto setTimeout = [&](HINTERNET handle, DWORD option) {
    DWORD value = static_cast<DWORD>(timeoutMs);
    if (!WinHttpSetOption(handle, option, &value, sizeof(value)))
      transportError(GetLastError(), bearer);
  };
  if (!WinHttpSetTimeouts(session.handle, timeoutMs, timeoutMs, timeoutMs,
                          timeoutMs))
    transportError(GetLastError(), bearer);
  setTimeout(session.handle, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT);
  Internet connection{WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0)};
  if (!connection.handle)
    transportError(GetLastError(), bearer);
  DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  Internet request{WinHttpOpenRequest(connection.handle, L"POST", path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
  if (!request.handle)
    transportError(GetLastError(), bearer);
  DWORD disable = WINHTTP_DISABLE_REDIRECTS;
  WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable,
                   sizeof(disable));
  if (!WinHttpSetTimeouts(request.handle, timeoutMs, timeoutMs, timeoutMs,
                          timeoutMs))
    transportError(GetLastError(), bearer);
  setTimeout(request.handle, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT);
  auto headers = std::wstring(L"Content-Type: application/json\r\nAuthorization: Bearer ") +
                 toWide(bearer) + L"\r\n";
  if (!WinHttpSendRequest(request.handle, headers.c_str(),
                           static_cast<DWORD>(headers.size()),
                           body.empty() ? WINHTTP_NO_REQUEST_DATA
                                        : const_cast<char *>(body.data()),
                           static_cast<DWORD>(body.size()),
                           static_cast<DWORD>(body.size()), 0) ||
      !WinHttpReceiveResponse(request.handle, nullptr))
    transportError(GetLastError(), bearer);
  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(request.handle,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX))
    transportError(GetLastError(), bearer);
  std::string response;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.handle, &available))
      transportError(GetLastError(), bearer);
    if (available == 0)
      break;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request.handle, chunk.data(), available, &read))
      transportError(GetLastError(), bearer);
    chunk.resize(read);
    response += chunk;
  }
  if (status < 200 || status >= 300) {
    auto detail = firstLine(response);
    auto message = "AI service returned " + std::to_string(status);
    if (!detail.empty())
      message += ": " + detail;
    throw Error("NETWORK_ERROR", redact(message, bearer));
  }
  return response;
}
} // namespace lexica::parity
