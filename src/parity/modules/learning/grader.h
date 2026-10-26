#pragma once
#include "parity/contracts/dto.h"
#include <cstdint>
#include <optional>
#include <string>
namespace lexica::parity::learning {
struct BuiltPrompt {
  PromptKind kind = PromptKind::Definition;
  std::string text;
  std::optional<std::int64_t> sense_id;
  std::optional<std::int64_t> example_id;
};
std::string normalizeWord(const std::string &input);
std::optional<std::string> blankWord(const std::string &text, const std::string &word);
std::string maskHint(const std::string &word);
bool isCorrect(const std::string &answer, const std::string &target);
std::size_t editDistance(const std::string &left, const std::string &right);
RecallQuality qualityFor(bool correct, std::int64_t hintsUsed);
BuiltPrompt buildPrompt(const WordEntry &entry);
BuiltPrompt buildDefinitionPrompt(const WordEntry &entry);
bool isBlank(const std::string &text);
} // namespace lexica::parity::learning
