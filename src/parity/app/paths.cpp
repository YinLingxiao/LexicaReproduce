#include "parity/app/paths.h"
#include "parity/contracts/error.h"
#include <fstream>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace lexica::parity {
namespace {
std::filesystem::path normalize(const std::filesystem::path &path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (!error)
    return canonical;
  return std::filesystem::absolute(path).lexically_normal();
}
std::wstring utf16(const std::string &text) {
  if (text.empty())
    return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (!size)
    throw Error("DATA_DIRECTORY_ERROR", "db_dir.txt is not valid UTF-8");
  std::wstring wide(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), wide.data(), size);
  return wide;
}
std::string utf8(const std::wstring &text) {
  if (text.empty())
    return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0, nullptr,
                          nullptr);
  if (!size)
    throw Error("DATA_DIRECTORY_ERROR", "Cannot encode the database directory");
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), out.data(), size, nullptr,
                      nullptr);
  return out;
}
std::string readPointer(const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  std::string text(std::istreambuf_iterator<char>(input), {});
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.erase(0, 3);
  const auto line = text.find_first_of("\r\n");
  if (line != std::string::npos)
    text.resize(line);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.pop_back();
  std::size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
    ++start;
  return text.substr(start);
}
void writePointer(const std::filesystem::path &file,
                  const std::filesystem::path &directory) {
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  if (!output)
    throw Error("DATA_DIRECTORY_ERROR", "Cannot write db_dir.txt");
  output << utf8(directory.wstring());
}
std::vector<std::filesystem::path> protectedRoots() {
  std::vector<std::filesystem::path> roots;
  if (const wchar_t *roaming = _wgetenv(L"APPDATA"))
    roots.push_back(std::filesystem::path(roaming) / L"com.lexica.app");
  if (const wchar_t *local = _wgetenv(L"LOCALAPPDATA"))
    roots.push_back(std::filesystem::path(local) / L"com.lexica.app");
  return roots;
}
} // namespace
std::filesystem::path appRoot() {
  const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
  if (!local || !*local)
    throw Error("DATA_DIRECTORY_ERROR",
                "LOCALAPPDATA is unavailable; specify --data-dir");
  return std::filesystem::path(local) / L"LexicaReproduce" / L"parity-v1";
}
bool isOriginalDataPath(const std::filesystem::path &path) {
  const auto candidate = normalize(path);
  for (const auto &root : protectedRoots()) {
    std::error_code missing;
    if (!std::filesystem::exists(root, missing))
      continue;
    const auto base = normalize(root);
    if (candidate.root_name() != base.root_name())
      continue;
    if (candidate == base)
      return true;
    std::error_code error;
    const auto relative = std::filesystem::relative(candidate, base, error);
    if (error || !relative.is_relative())
      continue;
    bool inside = !relative.empty();
    for (const auto &part : relative)
      if (part == L"..")
        inside = false;
    if (inside)
      return true;
  }
  return false;
}
std::filesystem::path originalDatabaseFile() {
  const wchar_t *roaming = _wgetenv(L"APPDATA");
  if (!roaming)
    return {};
  const auto configured = _wgetenv(L"LEXICA_UPSTREAM_DB");
  const auto file = configured && *configured
                        ? std::filesystem::path(configured)
                        : std::filesystem::path(roaming) / L"com.lexica.app" /
                              L"lexica.db";
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error))
    return {};
  return file;
}
DataLayout resolveLayout(const std::filesystem::path &root) {
  if (root.empty())
    throw Error("DATA_DIRECTORY_ERROR", "The data root is empty");
  const auto identity = normalize(root);
  if (isOriginalDataPath(identity))
    throw Error("DATA_DIRECTORY_ERROR",
                "Refusing to use the original Lexica data directory");
  std::filesystem::create_directories(identity);
  const auto pointer = identity / L"db_dir.txt";
  std::filesystem::path databaseDirectory = identity;
  if (std::filesystem::is_regular_file(pointer)) {
    const auto text = readPointer(pointer);
    if (!text.empty()) {
      auto target = std::filesystem::path(utf16(text));
      if (!target.is_absolute())
        throw Error("DATA_DIRECTORY_ERROR",
                    "db_dir.txt must contain an absolute directory");
      target = normalize(target);
      if (isOriginalDataPath(target))
        throw Error("DATA_DIRECTORY_ERROR",
                    "db_dir.txt points at the original Lexica data directory");
      databaseDirectory = target;
    }
  }
  std::filesystem::create_directories(databaseDirectory);
  const auto profile = identity / L"webview-profile";
  std::filesystem::create_directories(profile);
  if (!std::filesystem::exists(pointer))
    writePointer(pointer, databaseDirectory);
  return {identity, databaseDirectory, databaseDirectory / L"lexica.db", profile};
}
} // namespace lexica::parity
