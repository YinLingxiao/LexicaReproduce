#include "parity/modules/learning/grader.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <vector>
namespace lexica::parity::learning {
namespace {
struct Chars {
  std::vector<char32_t> data;
};
Chars decode(const std::string &text) {
  Chars out;
  const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = bytes[i];
    char32_t cp = c;
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp = (static_cast<char32_t>(c & 0x1F) << 6) |
           (bytes[i + 1] & 0x3F);
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp = (static_cast<char32_t>(c & 0x0F) << 12) |
           (static_cast<char32_t>(bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
      len = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
      cp = (static_cast<char32_t>(c & 0x07) << 18) |
           (static_cast<char32_t>(bytes[i + 1] & 0x3F) << 12) |
           (static_cast<char32_t>(bytes[i + 2] & 0x3F) << 6) | (bytes[i + 3] & 0x3F);
      len = 4;
    }
    out.data.push_back(cp);
    i += len;
  }
  return out;
}
void append(std::string &out, char32_t cp) {
  if (cp < 0x80)
    out.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}
std::string encode(const std::vector<char32_t> &chars, std::size_t begin,
                   std::size_t end) {
  std::string out;
  for (std::size_t i = begin; i < end; ++i)
    append(out, chars[i]);
  return out;
}
bool isSpace(char32_t cp) {
  if (cp < 128)
    return std::isspace(static_cast<unsigned char>(cp)) != 0;
  if (cp > 0xFFFF)
    return false;
  WORD type = 0;
  const wchar_t wc = static_cast<wchar_t>(cp);
  return GetStringTypeW(CT_CTYPE1, &wc, 1, &type) && (type & C1_SPACE);
}
char32_t lowerCp(char32_t cp) {
  if (cp < 128)
    return static_cast<char32_t>(std::tolower(static_cast<unsigned char>(cp)));
  if (cp > 0xFFFF)
    return cp;
  wchar_t wc = static_cast<wchar_t>(cp);
  CharLowerBuffW(&wc, 1);
  return static_cast<char32_t>(wc);
}
bool isAlnum(char32_t cp) {
  if (cp < 128)
    return std::isalnum(static_cast<unsigned char>(cp)) != 0;
  if (cp > 0xFFFF)
    return false;
  WORD type = 0;
  const wchar_t wc = static_cast<wchar_t>(cp);
  return GetStringTypeW(CT_CTYPE1, &wc, 1, &type) &&
         (type & (C1_ALPHA | C1_DIGIT));
}
std::size_t distance(const std::vector<char32_t> &left,
                     const std::vector<char32_t> &right) {
  std::vector<std::size_t> previous(right.size() + 1), current(right.size() + 1);
  for (std::size_t j = 0; j <= right.size(); ++j)
    previous[j] = j;
  for (std::size_t i = 1; i <= left.size(); ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= right.size(); ++j) {
      const auto cost = left[i - 1] == right[j - 1] ? 0 : 1;
      current[j] = std::min(previous[j] + 1,
                            std::min(current[j - 1] + 1, previous[j - 1] + cost));
    }
    previous.swap(current);
  }
  return previous[right.size()];
}
} // namespace
std::string normalizeWord(const std::string &input) {
  auto chars = decode(input).data;
  for (auto &cp : chars)
    if (cp >= 0xFF01 && cp <= 0xFF5E)
      cp -= 0xFEE0;
  std::size_t begin = 0, end = chars.size();
  while (begin < end && isSpace(chars[begin]))
    ++begin;
  while (end > begin && isSpace(chars[end - 1]))
    --end;
  std::string lowered = encode(chars, begin, end);
  auto wide = std::wstring();
  if (!lowered.empty()) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, lowered.data(),
                                         static_cast<int>(lowered.size()), nullptr, 0);
    wide.resize(size);
    MultiByteToWideChar(CP_UTF8, 0, lowered.data(), static_cast<int>(lowered.size()),
                        wide.data(), size);
    if (!wide.empty())
      CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
    lowered.resize(bytes);
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        lowered.data(), bytes, nullptr, nullptr);
  }
  std::string quotes;
  quotes.reserve(lowered.size());
  auto cps = decode(lowered).data;
  for (auto cp : cps) {
    if (cp == 0x2018 || cp == 0x2019 || cp == '`' || cp == 0x00B4)
      cp = '\'';
    else if (cp == 0x201C || cp == 0x201D)
      cp = '"';
    append(quotes, cp);
  }
  auto trimmed = decode(quotes).data;
  begin = 0;
  end = trimmed.size();
  while (begin < end && (trimmed[begin] == '\'' || trimmed[begin] == '"'))
    ++begin;
  while (end > begin && (trimmed[end - 1] == '\'' || trimmed[end - 1] == '"'))
    --end;
  return encode(trimmed, begin, end);
}
std::optional<std::string> blankWord(const std::string &text, const std::string &word) {
  auto wordChars = decode(word).data;
  for (auto &cp : wordChars)
    cp = lowerCp(cp);
  if (wordChars.empty())
    return std::nullopt;
  auto textChars = decode(text).data;
  if (textChars.size() < wordChars.size())
    return std::nullopt;
  std::string out;
  bool found = false;
  std::size_t start = 0;
  while (start < textChars.size()) {
    const auto end = start + wordChars.size();
    if (end > textChars.size()) {
      out += encode(textChars, start, textChars.size());
      break;
    }
    const bool boundary =
        (start == 0 || !isAlnum(textChars[start - 1])) &&
        (end == textChars.size() || !isAlnum(textChars[end]));
    if (!boundary) {
      append(out, textChars[start]);
      ++start;
      continue;
    }
    bool matched = true;
    for (std::size_t j = 0; j < wordChars.size(); ++j) {
      if (lowerCp(textChars[start + j]) != wordChars[j]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      out += "______";
      found = true;
      start = end;
    } else {
      append(out, textChars[start]);
      ++start;
    }
  }
  if (!found)
    return std::nullopt;
  return out;
}
std::string maskHint(const std::string &word) {
  auto chars = decode(word).data;
  if (chars.empty())
    return {};
  std::string out;
  append(out, chars[0]);
  out.append(chars.size() - 1, '_');
  return out;
}
std::size_t editDistance(const std::string &left, const std::string &right) {
  return distance(decode(left).data, decode(right).data);
}
bool isCorrect(const std::string &answer, const std::string &target) {
  const auto left = normalizeWord(answer);
  const auto right = normalizeWord(target);
  if (left.empty() || right.empty())
    return false;
  if (left == right)
    return true;
  const auto a = decode(left).data;
  const auto b = decode(right).data;
  if (b.size() < 5)
    return false;
  const auto gap = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
  return gap <= 1 && distance(a, b) <= 1;
}
RecallQuality qualityFor(bool correct, std::int64_t hintsUsed) {
  if (!correct)
    return RecallQuality::Forgotten;
  if (hintsUsed <= 0)
    return RecallQuality::Excellent;
  if (hintsUsed == 1)
    return RecallQuality::Good;
  if (hintsUsed == 2)
    return RecallQuality::Hard;
  return RecallQuality::Poor;
}
BuiltPrompt buildDefinitionPrompt(const WordEntry &entry) {
  for (const auto &sense : entry.senses) {
    if (sense.chinese_definition && !isBlank(*sense.chinese_definition)) {
      const auto zh = *sense.chinese_definition;
      auto text = blankWord(zh, entry.word).value_or(zh);
      return {PromptKind::DefinitionZh, std::move(text), sense.id, std::nullopt};
    }
  }
  for (const auto &sense : entry.senses) {
    if (!isBlank(sense.english_definition)) {
      auto text = blankWord(sense.english_definition, entry.word)
                      .value_or(sense.english_definition);
      return {PromptKind::Definition, std::move(text), sense.id, std::nullopt};
    }
  }
  return {};
}
BuiltPrompt buildPrompt(const WordEntry &entry) {
  for (const auto &sense : entry.senses) {
    for (const auto &example : sense.examples) {
      if (auto blanked = blankWord(example.text, entry.word))
        return {PromptKind::Cloze, *blanked, sense.id, example.id};
    }
  }
  for (const auto &sense : entry.senses) {
    if (!isBlank(sense.english_definition)) {
      auto text = blankWord(sense.english_definition, entry.word)
                      .value_or(sense.english_definition);
      return {PromptKind::Definition, std::move(text), sense.id, std::nullopt};
    }
  }
  return buildDefinitionPrompt(entry);
}
bool isBlank(const std::string &text) {
  auto chars = decode(text).data;
  return std::none_of(chars.begin(), chars.end(),
                      [](char32_t cp) { return !isSpace(cp); });
}
} // namespace lexica::parity::learning
