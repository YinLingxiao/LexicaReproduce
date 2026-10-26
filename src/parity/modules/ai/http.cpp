#include "parity/modules/ai/http.h"
#include "parity/api/http_request.h"
#include "parity/contracts/error.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
namespace lexica::parity {
namespace {
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
  for (std::size_t at = 0;
       (at = message.find(secret, at)) != std::string::npos;) {
    message.replace(at, secret.size(), "[redacted]");
    at += 10;
  }
  return message;
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
  HttpRequest request(url, L"POST", timeoutMs, useProxy_, false);
  const auto headers =
      std::wstring(
          L"Content-Type: application/json\r\nAuthorization: Bearer ") +
      toWide(bearer) + L"\r\n";
  request.start(headers, body);
  const auto status = request.status();
  std::string response;
  for (;;) {
    auto chunk = request.read();
    if (chunk.empty())
      break;
    response += chunk;
    if (response.size() > 8 * 1024 * 1024)
      throw Error("NETWORK_ERROR", "AI response is too large");
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
