#pragma once
#include <cstddef>
#include <string>
#include <string_view>
namespace lexica::parity {
std::string normalizeWord(std::string_view input);
std::string prefixUpperBound(std::string_view prefix);
std::string trimCopy(std::string_view input);
std::string ftsQuoteTerm(std::string_view term);
std::size_t utf8Length(std::string_view text);
bool endsWith(std::string_view text, std::string_view suffix);
}
