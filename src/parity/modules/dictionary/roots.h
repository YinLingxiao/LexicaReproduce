#pragma once
#include "parity/contracts/dto.h"
#include <optional>
#include <string>
#include <tuple>
#include <vector>
namespace lexica::parity {
struct FallbackGloss {
  const char *pos;
  const char *gloss;
};
std::size_t affixRuleCount();
std::vector<RootPart> decomposeRoots(const std::string &word);
std::size_t levenshtein(const std::string &left, const std::string &right);
std::tuple<std::string, std::optional<std::string>, std::optional<std::string>>
parseWordFamilyEntry(const std::string &raw);
bool isPosToken(const std::string &token);
std::pair<std::optional<std::string>, std::string>
extractPosAndCleanGloss(const std::optional<std::string> &pos,
                        const std::string *chinese, const std::string *english);
const FallbackGloss *fallbackRelatedGloss(const std::string &word);
}
