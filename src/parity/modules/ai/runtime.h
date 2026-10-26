#pragma once
#include "parity/contracts/ports.h"
#include <string>
namespace lexica::parity {
void setAiRuntime(KeyStore *keys, HttpClient *http);
void resetAiRuntime();
std::string aiTraceLog();
void clearAiTrace();
} // namespace lexica::parity
