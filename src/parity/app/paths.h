#pragma once
#include <filesystem>
namespace lexica::parity {
struct DataLayout {
  std::filesystem::path root;
  std::filesystem::path databaseDirectory;
  std::filesystem::path databaseFile;
  std::filesystem::path profile;
};
std::filesystem::path appRoot();
DataLayout resolveLayout(const std::filesystem::path &root);
bool isOriginalDataPath(const std::filesystem::path &path);
std::filesystem::path originalDatabaseFile();
} // namespace lexica::parity
