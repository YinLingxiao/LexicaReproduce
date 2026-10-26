#pragma once
#include "parity/contracts/dto.h"
#include <filesystem>
namespace lexica::parity {
Json probeUpstreamDatabase(const std::filesystem::path &file);
} // namespace lexica::parity
