#pragma once
#include "parity/contracts/dto.h"
#include <optional>
#include <string>
#include <vector>
namespace lexica::parity {
inline constexpr int kAiPromptVersion = 2;
inline constexpr int kAiTimeoutMs = 15000;
inline constexpr const char *kDefaultAiBaseUrl = "https://api.deepseek.com/v1";
inline constexpr const char *kDefaultAiModel = "deepseek-chat";
struct GenWord {
  std::int64_t word_id = 0;
  std::optional<std::int64_t> sense_id;
  std::string word;
  std::string english;
  std::string chinese;
};
std::string trimText(const std::string &value);
std::string normalizeWord(const std::string &input);
std::optional<std::string> blankWord(const std::string &text, const std::string &word);
std::vector<Json> parseJsonItems(const std::string &content);
std::vector<AiExample> validateAndBlank(const std::string &content,
                                        const std::vector<GenWord> &words);
} // namespace lexica::parity
