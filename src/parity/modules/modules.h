#pragma once
#include "parity/api/router.h"
#include <memory>
namespace lexica::parity {
std::unique_ptr<DictionaryPort> makeDictionary();
void registerDictionary(Router &);
void registerLearning(Router &);
void registerLibrary(Router &);
void registerAi(Router &);
void registerTts(Router &);
} // namespace lexica::parity
