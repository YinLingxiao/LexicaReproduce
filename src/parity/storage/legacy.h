#pragma once
#include "parity/contracts/clock.h"
#include "parity/contracts/dto.h"
#include "parity/storage/database.h"
#include <filesystem>
namespace lexica::parity {
Json inspectLegacy(const std::filesystem::path &source);
Json migrateLegacy(const std::filesystem::path &source, Database &target,
                   const Clock &clock);
} // namespace lexica::parity
