#include "parity/modules/dictionary/api.h"
#include <cstdio>
#include <fstream>
#include <mutex>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
namespace lexica::parity {
namespace {
const char *kDefaultUrls[] = {
    "https://github.com/skywind3000/ECDICT/releases/download/1.0.28/ecdict-sqlite-28.zip",
    "https://downloads.sourceforge.net/project/ecdict.mirror/1.0.28/ecdict-sqlite-28.zip"};
using Emit = std::function<void(SetupPhase, std::optional<double>, std::string)>;
bool largeFile(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         std::filesystem::file_size(path, error) > 1000000;
}
std::wstring toWide(const std::string &text) {
  if (text.empty())
    return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0)
    throw Error("BAD_REQUEST", "URL is not valid UTF-8");
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), out.data(), size);
  return out;
}
std::wstring quoteArg(const std::wstring &text) {
  if (text.find(L'"') != std::wstring::npos)
    throw Error("RESOURCE_ERROR", "Dictionary path contains a quote");
  return L"\"" + text + L"\"";
}
std::string runProcess(const std::wstring &command, const std::filesystem::path &cwd) {
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  wchar_t directory[MAX_PATH];
  wchar_t fileName[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"lex", 0, fileName))
    throw Error("RESOURCE_ERROR", "Cannot capture extractor output");
  HANDLE file = CreateFileW(fileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &security,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw Error("RESOURCE_ERROR", "Cannot capture extractor output");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = file;
  startup.hStdError = file;
  PROCESS_INFORMATION process{};
  std::wstring mutableCommand = command;
  const auto started =
      CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                     nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
  if (!started) {
    CloseHandle(file);
    DeleteFileW(fileName);
    throw Error("RESOURCE_ERROR", "Cannot start tar.exe");
  }
  if (WaitForSingleObject(process.hProcess, 600000) != WAIT_OBJECT_0)
    TerminateProcess(process.hProcess, 1);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  SetFilePointer(file, 0, nullptr, FILE_BEGIN);
  std::string output;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read)
    output.append(buffer, buffer + read);
  CloseHandle(file);
  DeleteFileW(fileName);
  if (code != 0)
    throw Error("RESOURCE_ERROR", "Dictionary archive could not be extracted");
  return output;
}
bool safeMember(const std::string &name) {
  if (name.empty() || name.front() == '/' || name.front() == '\\' ||
      name.find(':') != std::string::npos)
    return false;
  std::size_t index = 0;
  while (index < name.size()) {
    const auto end = name.find_first_of("/\\", index);
    const auto part = name.substr(index, end == std::string::npos ? std::string::npos : end - index);
    if (part == "..")
      return false;
    if (end == std::string::npos)
      break;
    index = end + 1;
  }
  return true;
}
std::string baseName(std::string name) {
  while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
    name.pop_back();
  const auto slash = name.find_last_of("/\\");
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  return name;
}
bool isDbName(const std::string &name) {
  if (name.size() >= 3) {
    auto ext = name.substr(name.size() - 3);
    for (char &ch : ext)
      if (ch >= 'A' && ch <= 'Z')
        ch = static_cast<char>(ch - 'A' + 'a');
    if (ext == ".db")
      return true;
  }
  std::string lower = name;
  for (char &ch : lower)
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
  return lower == "stardict.db";
}
void downloadOne(const std::string &url, const std::filesystem::path &dest, const Emit &emit) {
  const auto wide = toWide(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256];
  wchar_t path[4096];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
    throw Error("NETWORK_ERROR", "Invalid dictionary URL");
  const bool loopback = _wcsicmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0;
  HINTERNET session = WinHttpOpen(
      L"Lexica/0.1 (dictionary bootstrap)",
      loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    throw Error("NETWORK_ERROR", "Cannot open HTTP session");
  struct Internet {
    HINTERNET handle;
    ~Internet() {
      if (handle)
        WinHttpCloseHandle(handle);
    }
  };
  Internet sessionHandle{session};
  WinHttpSetTimeouts(session, 600000, 600000, 600000, 600000);
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
  if (!connection)
    throw Error("NETWORK_ERROR", "Cannot connect to dictionary host");
  Internet connectionHandle{connection};
  const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request =
      WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request)
    throw Error("NETWORK_ERROR", "Cannot open dictionary request");
  Internet requestHandle{request};
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr))
    throw Error("NETWORK_ERROR", "Dictionary download failed");
  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
  if (status < 200 || status >= 300)
    throw Error("NETWORK_ERROR", "Dictionary download returned HTTP " + std::to_string(status));
  std::optional<std::uint64_t> total;
  wchar_t lengthText[64];
  DWORD lengthSize = sizeof(lengthText);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                          lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX))
    total = std::wcstoull(lengthText, nullptr, 10);
  const auto partial = dest.wstring() + L".part";
  HANDLE output = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE)
    throw Error("RESOURCE_ERROR", "Cannot create dictionary download");
  struct File {
    HANDLE handle;
    ~File() {
      if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    }
  } file{output};
  std::uint64_t written = 0;
  std::uint64_t lastEmit = 0;
  for (;;) {
    char buffer[65536];
    DWORD got = 0;
    if (!WinHttpReadData(request, buffer, sizeof(buffer), &got))
      throw Error("NETWORK_ERROR", "Dictionary download failed");
    if (got == 0)
      break;
    DWORD stored = 0;
    if (!WriteFile(output, buffer, got, &stored, nullptr) || stored != got)
      throw Error("RESOURCE_ERROR", "Cannot write dictionary download");
    written += got;
    if (written - lastEmit > 2 * 1024 * 1024 || (total && written >= *total)) {
      lastEmit = written;
      const double mb = static_cast<double>(written) / (1024.0 * 1024.0);
      std::optional<double> progress;
      std::string message;
      if (total) {
        progress = static_cast<double>(written) / static_cast<double>(*total);
        const double totalMb = static_cast<double>(*total) / (1024.0 * 1024.0);
        char formatted[160];
        std::snprintf(formatted, sizeof(formatted), "Downloading dictionary\xE2\x80\xA6 %.0f / %.0f MB",
                      mb, totalMb);
        message = formatted;
      } else {
        char formatted[160];
        std::snprintf(formatted, sizeof(formatted), "Downloading dictionary\xE2\x80\xA6 %.0f MB", mb);
        message = formatted;
      }
      emit(SetupPhase::Downloading, progress, message);
    }
  }
  CloseHandle(output);
  file.handle = INVALID_HANDLE_VALUE;
  std::error_code error;
  std::filesystem::remove(dest, error);
  if (!MoveFileW(partial.c_str(), dest.c_str()))
    throw Error("RESOURCE_ERROR", "Cannot store dictionary download");
}
void downloadZip(const std::vector<std::string> &urls, const std::filesystem::path &dest,
                 const Emit &emit) {
  std::string last = "no dictionary URL configured";
  for (const auto &url : urls) {
    emit(SetupPhase::Downloading, 0.0, "Downloading dictionary\xE2\x80\xA6\n" + url);
    try {
      downloadOne(url, dest, emit);
      return;
    } catch (const Error &error) {
      last = error.what();
      std::error_code ignore;
      std::filesystem::remove(dest, ignore);
      std::filesystem::remove(std::filesystem::path(dest.wstring() + L".part"), ignore);
    }
  }
  throw Error("NETWORK_ERROR", "dictionary download failed: " + last);
}
void extractStardict(const std::filesystem::path &zip, const std::filesystem::path &dest,
                     const Emit &emit) {
  emit(SetupPhase::Extracting, std::nullopt, "Extracting dictionary\xE2\x80\xA6");
  const auto tar = std::filesystem::path(L"C:\\Windows\\System32\\tar.exe");
  if (!std::filesystem::exists(tar))
    throw Error("RESOURCE_ERROR", "tar.exe is required to extract the dictionary");
  const auto listing = runProcess(tar.wstring() + L" -tf " + quoteArg(zip.wstring()), {});
  std::string member;
  std::size_t index = 0;
  while (index < listing.size()) {
    auto end = listing.find('\n', index);
    auto line = listing.substr(index, end == std::string::npos ? std::string::npos : end - index);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    index = end == std::string::npos ? listing.size() : end + 1;
    if (!safeMember(line) || !isDbName(baseName(line)))
      continue;
    member = line;
    if (baseName(line) == "stardict.db" || baseName(line) == "stardict.DB")
      break;
  }
  if (member.empty())
    throw Error("RESOURCE_ERROR", "stardict.db not found inside the downloaded archive");
  const auto temp = dest.parent_path() / "ecdict-extract";
  std::error_code error;
  std::filesystem::remove_all(temp, error);
  std::filesystem::create_directories(temp);
  runProcess(tar.wstring() + L" -xf " + quoteArg(zip.wstring()) + L" -C " +
                 quoteArg(temp.wstring()) + L" " + quoteArg(toWide(member)),
             {});
  std::filesystem::path extracted;
  for (const auto &item : std::filesystem::recursive_directory_iterator(temp))
    if (item.is_regular_file() && isDbName(item.path().filename().string())) {
      extracted = item.path();
      break;
    }
  if (extracted.empty())
    throw Error("RESOURCE_ERROR", "stardict.db not found inside the downloaded archive");
  const auto partial = std::filesystem::path(dest.wstring() + L".part");
  std::filesystem::remove(partial, error);
  std::filesystem::rename(extracted, partial);
  std::filesystem::remove(dest, error);
  std::filesystem::rename(partial, dest);
  std::filesystem::remove_all(temp, error);
}
void writeMarker(const std::filesystem::path &dir) {
  std::ofstream output(dir / "ecdict.ok", std::ios::binary);
  output << "1";
}
} // namespace
EnsureDictionaryResult ensureDictionary(Context &context, const EnsureOptions &options) {
  static std::mutex setupLock;
  std::lock_guard<std::mutex> guard(setupLock);
  Emit emit = [&](SetupPhase phase, std::optional<double> progress, std::string message) {
    if (!context.emit)
      return;
    SetupProgress event;
    event.phase = phase;
    event.progress = progress;
    event.message = std::move(message);
    context.emit("dictionary-setup", event);
  };
  auto dir = context.db.path().parent_path();
  if (dir.empty())
    dir = ".";
  std::filesystem::create_directories(dir);
  emit(SetupPhase::Checking, std::nullopt, "Checking dictionary\xE2\x80\xA6");
  const auto now = formatUtc(context.clock.nowMillis());
  const auto counts = context.db.withConnection([&](sqlite3 *db) {
    backfillSeedMetadata(db, now);
    Statement words(db, "SELECT COUNT(*) FROM words");
    words.step();
    Statement ecdict(db, "SELECT COUNT(*) FROM words WHERE source='ecdict'");
    ecdict.step();
    return std::pair<std::int64_t, std::int64_t>{words.integer(0), ecdict.integer(0)};
  });
  if (counts.second >= kDictionaryReadyMinEntries) {
    writeMarker(dir);
    emit(SetupPhase::Ready, 1.0, "Dictionary ready");
    return {true, counts.first, 0, true};
  }
  const auto zip = dir / "ecdict-sqlite-28.zip";
  const auto stardict = dir / "stardict.db";
  std::vector<std::string> urls(std::begin(kDefaultUrls), std::end(kDefaultUrls));
  if (!options.urls.empty())
    urls = options.urls;
  if (!largeFile(zip))
    downloadZip(urls, zip, emit);
  if (!largeFile(stardict))
    extractStardict(zip, stardict, emit);
  emit(SetupPhase::Importing, std::nullopt,
       "Importing entries \xE2\x80\x94 this can take several minutes\xE2\x80\xA6");
  auto connection = openConfiguredDatabase(context.db.path());
  const auto imported = importEcdictDatabase(connection.get(), stardict, now);
  Statement words(connection.get(), "SELECT COUNT(*) FROM words");
  words.step();
  Statement ecdict(connection.get(), "SELECT COUNT(*) FROM words WHERE source='ecdict'");
  ecdict.step();
  const bool ready = ecdict.integer(0) >= kDictionaryReadyMinEntries;
  if (ready)
    writeMarker(dir);
  std::error_code ignore;
  std::filesystem::remove(zip, ignore);
  emit(SetupPhase::Ready, 1.0,
       "Dictionary ready \xC2\xB7 " + std::to_string(words.integer(0)) + " entries");
  return {ready, words.integer(0), imported, false};
}
} // namespace lexica::parity
