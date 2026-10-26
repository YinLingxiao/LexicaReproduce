#pragma once
#include "parity/contracts/cancel.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace lexica::parity {
// WinHTTP runs asynchronously; this adapter waits cooperatively with one total
// deadline. Closing an asynchronous handle cancels I/O safely. The callback
// context remains alive until HANDLE_CLOSING (see WinHttpCloseHandle docs).
class HttpRequest {
  HINTERNET session_ = nullptr, connection_ = nullptr, request_ = nullptr;
  HANDLE closed_ = nullptr;
  bool callbackBound_ = false;
  const Cancellation *cancellation_;
  std::chrono::steady_clock::time_point deadline_;
  std::mutex mutex_;
  std::condition_variable ready_;
  DWORD completed_ = 0, transferred_ = 0, error_ = 0;
  std::string body_;
  // Async I/O may still access this buffer while cancellation unwinds read().
  // Keep it alive until close() has observed HANDLE_CLOSING.
  std::string readBuffer_;

  static std::wstring wide(const std::string &value) {
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
      throw Error("NETWORK_ERROR", "Invalid request encoding");
    std::wstring out(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), length);
    return out;
  }
  static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status,
                                LPVOID info, DWORD length) {
    if (!context)
      return;
    auto *self = reinterpret_cast<HttpRequest *>(context);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
      SetEvent(self->closed_);
      return;
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
      self->error_ = static_cast<WINHTTP_ASYNC_RESULT *>(info)->dwError;
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
      self->completed_ = status;
      self->transferred_ = status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE
                               ? *static_cast<DWORD *>(info)
                               : length;
    }
    self->ready_.notify_all();
  }
  void close() noexcept {
    if (request_) {
      WinHttpCloseHandle(request_);
      request_ = nullptr;
      if (callbackBound_)
        WaitForSingleObject(closed_, INFINITE);
    }
    if (connection_) {
      WinHttpCloseHandle(connection_);
      connection_ = nullptr;
    }
    if (session_) {
      WinHttpCloseHandle(session_);
      session_ = nullptr;
    }
    if (closed_) {
      CloseHandle(closed_);
      closed_ = nullptr;
    }
  }
  [[noreturn]] void failed(DWORD code) {
    check();
    if (code == ERROR_WINHTTP_TIMEOUT)
      throw Error("NETWORK_ERROR", "Request timed out");
    throw Error("NETWORK_ERROR",
                "HTTP request failed: " + std::to_string(code));
  }
  void begin() {
    check();
    std::lock_guard<std::mutex> lock(mutex_);
    completed_ = 0;
    transferred_ = 0;
    error_ = 0;
  }
  void accepted(BOOL result) {
    if (!result) {
      const auto code = GetLastError();
      if (code != ERROR_IO_PENDING)
        failed(code);
    }
  }
  DWORD wait(DWORD expected) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (completed_ != expected && !error_) {
      ready_.wait_for(lock, std::chrono::milliseconds(20));
      check();
    }
    if (error_) {
      const auto error = error_;
      lock.unlock();
      failed(error);
    }
    check();
    return transferred_;
  }

public:
  HttpRequest(const std::string &url, const wchar_t *method, int timeoutMs,
              bool useProxy, bool redirects)
      : cancellation_(currentCancellation()),
        deadline_(std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeoutMs)) {
    try {
      check();
      closed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!closed_)
        throw Error("NETWORK_ERROR", "Cannot create request completion event");
      const auto uri = wide(url);
      URL_COMPONENTS parts{};
      parts.dwStructSize = sizeof(parts);
      std::wstring host(512, L'\0'), path(8192, L'\0'), extra(8192, L'\0');
      parts.lpszHostName = host.data();
      parts.dwHostNameLength = static_cast<DWORD>(host.size());
      parts.lpszUrlPath = path.data();
      parts.dwUrlPathLength = static_cast<DWORD>(path.size());
      parts.lpszExtraInfo = extra.data();
      parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
      if (!WinHttpCrackUrl(uri.c_str(), static_cast<DWORD>(uri.size()), 0,
                           &parts))
        throw Error("NETWORK_ERROR", "Invalid HTTP URL");
      if (parts.nScheme != INTERNET_SCHEME_HTTP &&
          parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw Error("NETWORK_ERROR", "Unsupported HTTP URL");
      host.resize(parts.dwHostNameLength);
      path.resize(parts.dwUrlPathLength);
      extra.resize(parts.dwExtraInfoLength);
      if (path.empty())
        path = L"/";
      path += extra;
      session_ = WinHttpOpen(L"LexicaReproduce/1.1",
                             useProxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                      : WINHTTP_ACCESS_TYPE_NO_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                             WINHTTP_FLAG_ASYNC);
      if (!session_)
        failed(GetLastError());
      if (!WinHttpSetTimeouts(session_, timeoutMs, timeoutMs, timeoutMs,
                              timeoutMs))
        failed(GetLastError());
      connection_ = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
      if (!connection_)
        failed(GetLastError());
      request_ = WinHttpOpenRequest(
          connection_, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
          WINHTTP_DEFAULT_ACCEPT_TYPES,
          parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
      if (!request_)
        failed(GetLastError());
      DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
      if (!WinHttpSetOption(request_, WINHTTP_OPTION_CONTEXT_VALUE, &context,
                            sizeof(context)))
        failed(GetLastError());
      if (WinHttpSetStatusCallback(request_, callback,
                                   WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                       WINHTTP_CALLBACK_FLAG_HANDLES,
                                   0) == WINHTTP_INVALID_STATUS_CALLBACK)
        failed(GetLastError());
      callbackBound_ = true;
      if (!redirects) {
        DWORD flags = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(request_, WINHTTP_OPTION_DISABLE_FEATURE, &flags,
                              sizeof(flags)))
          failed(GetLastError());
      }
    } catch (...) {
      close();
      throw;
    }
  }
  ~HttpRequest() { close(); }
  HttpRequest(const HttpRequest &) = delete;
  HttpRequest &operator=(const HttpRequest &) = delete;
  void check() const {
    throwIfCancelled(cancellation_);
    if (std::chrono::steady_clock::now() >= deadline_)
      throw Error("NETWORK_ERROR", "Request timed out");
  }
  void start(const std::wstring &headers = {}, std::string body = {}) {
    body_ = std::move(body);
    begin();
    accepted(WinHttpSendRequest(
        request_,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        static_cast<DWORD>(headers.size()),
        body_.empty() ? WINHTTP_NO_REQUEST_DATA : body_.data(),
        static_cast<DWORD>(body_.size()), static_cast<DWORD>(body_.size()),
        reinterpret_cast<DWORD_PTR>(this)));
    wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    begin();
    accepted(WinHttpReceiveResponse(request_, nullptr));
    wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
  }
  DWORD status() {
    check();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(
            request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
            WINHTTP_NO_HEADER_INDEX))
      failed(GetLastError());
    return status;
  }
  std::optional<std::uint64_t> contentLength() {
    check();
    wchar_t buffer[64]{};
    DWORD length = sizeof(buffer);
    if (!WinHttpQueryHeaders(request_, WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX, buffer, &length,
                             WINHTTP_NO_HEADER_INDEX))
      return std::nullopt;
    return std::wcstoull(buffer, nullptr, 10);
  }
  std::string read() {
    begin();
    accepted(WinHttpQueryDataAvailable(request_, nullptr));
    const auto available = wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE);
    if (!available)
      return {};
    readBuffer_.assign(std::min<DWORD>(available, 65536), '\0');
    begin();
    accepted(WinHttpReadData(request_, readBuffer_.data(),
                             static_cast<DWORD>(readBuffer_.size()), nullptr));
    readBuffer_.resize(wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE));
    return readBuffer_;
  }
};
} // namespace lexica::parity
