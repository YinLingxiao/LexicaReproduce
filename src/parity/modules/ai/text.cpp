#include "parity/modules/ai/text.h"
#include "parity/contracts/error.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cctype>
namespace lexica::parity {
namespace {
std::wstring toWide(const std::string &value) {
  if (value.empty())
    return {};
  auto n = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                               static_cast<int>(value.size()), nullptr, 0);
  if (n <= 0)
    throw Error("BAD_REQUEST", "Text is not valid UTF-8");
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), n);
  return out;
}
std::string toUtf8(const std::wstring &value) {
  if (value.empty())
    return {};
  auto n = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                               static_cast<int>(value.size()), nullptr, 0,
                               nullptr, nullptr);
  if (n <= 0)
    throw Error("BAD_REQUEST", "Text could not be encoded");
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}
std::wstring nfkc(const std::wstring &value) {
  if (value.empty())
    return {};
  int estimate = NormalizeString(NormalizationKC, value.c_str(),
                                 static_cast<int>(value.size()), nullptr, 0);
  if (estimate <= 0)
    return value;
  std::wstring out(static_cast<std::size_t>(estimate), L'\0');
  int written = NormalizeString(NormalizationKC, value.c_str(),
                                static_cast<int>(value.size()), out.data(),
                                estimate);
  if (written < 0) {
    estimate = -written;
    out.assign(static_cast<std::size_t>(estimate), L'\0');
    written = NormalizeString(NormalizationKC, value.c_str(),
                              static_cast<int>(value.size()), out.data(),
                              estimate);
  }
  if (written <= 0)
    return value;
  out.resize(static_cast<std::size_t>(written));
  return out;
}
std::wstring lowerWide(const std::wstring &value) {
  if (value.empty())
    return {};
  int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.c_str(),
                        static_cast<int>(value.size()), nullptr, 0, nullptr,
                        nullptr, 0);
  if (n <= 0)
    return value;
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                              value.c_str(), static_cast<int>(value.size()),
                              out.data(), n, nullptr, nullptr, 0);
  if (written <= 0)
    return value;
  out.resize(static_cast<std::size_t>(written));
  return out;
}
bool isSpace(char32_t c) {
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
    return true;
  if (c > 0xFFFF)
    return false;
  WORD type = 0;
  auto ch = static_cast<wchar_t>(c);
  if (!GetStringTypeW(CT_CTYPE1, &ch, 1, &type))
    return false;
  return (type & C1_SPACE) != 0;
}
bool isAlnum(char32_t c) {
  if (c < 128)
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  if (c > 0xFFFF)
    return false;
  WORD type = 0;
  auto ch = static_cast<wchar_t>(c);
  if (!GetStringTypeW(CT_CTYPE1, &ch, 1, &type))
    return false;
  return (type & (C1_ALPHA | C1_DIGIT)) != 0;
}
std::u32string toU32(const std::string &value) {
  std::u32string out;
  for (std::size_t i = 0; i < value.size();) {
    auto c = static_cast<unsigned char>(value[i]);
    char32_t cp = 0xFFFD;
    std::size_t n = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < value.size()) {
      cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(value[i + 1]) & 0x3F);
      n = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < value.size()) {
      cp = ((c & 0x0F) << 12) |
           ((static_cast<unsigned char>(value[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(value[i + 2]) & 0x3F);
      n = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < value.size()) {
      cp = ((c & 0x07) << 18) |
           ((static_cast<unsigned char>(value[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(value[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(value[i + 3]) & 0x3F);
      n = 4;
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}
void appendCp(std::string &out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
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
std::string fromU32(const std::u32string &value) {
  std::string out;
  for (auto cp : value)
    appendCp(out, cp);
  return out;
}
std::u32string lowerCp(char32_t cp) {
  return toU32(toUtf8(lowerWide(toWide(fromU32(std::u32string(1, cp))))));
}
std::size_t whitespaceWords(const std::u32string &value) {
  std::size_t count = 0;
  bool in = false;
  for (auto cp : value) {
    if (isSpace(cp)) {
      in = false;
    } else if (!in) {
      in = true;
      ++count;
    }
  }
  return count;
}
const GenWord *findWord(const std::vector<GenWord> &words, std::int64_t id) {
  for (const auto &word : words)
    if (word.word_id == id)
      return &word;
  return nullptr;
}
} // namespace
std::string trimText(const std::string &value) {
  auto chars = toU32(value);
  std::size_t begin = 0;
  auto end = chars.size();
  while (begin < end && isSpace(chars[begin]))
    ++begin;
  while (end > begin && isSpace(chars[end - 1]))
    --end;
  return fromU32(chars.substr(begin, end - begin));
}
std::string normalizeWord(const std::string &input) {
  auto value = nfkc(toWide(input));
  std::size_t begin = 0;
  auto end = value.size();
  while (begin < end && isSpace(static_cast<char32_t>(value[begin])))
    ++begin;
  while (end > begin && isSpace(static_cast<char32_t>(value[end - 1])))
    --end;
  value = lowerWide(value.substr(begin, end - begin));
  for (auto &ch : value) {
    if (ch == 0x2018 || ch == 0x2019 || ch == L'`' || ch == 0x00B4)
      ch = L'\'';
    else if (ch == 0x201C || ch == 0x201D)
      ch = L'"';
  }
  begin = 0;
  end = value.size();
  while (begin < end && (value[begin] == L'\'' || value[begin] == L'"'))
    ++begin;
  while (end > begin && (value[end - 1] == L'\'' || value[end - 1] == L'"'))
    --end;
  return toUtf8(value.substr(begin, end - begin));
}
std::optional<std::string> blankWord(const std::string &text,
                                     const std::string &word) {
  auto wordChars = toU32(toUtf8(lowerWide(toWide(word))));
  auto textChars = toU32(text);
  if (wordChars.empty() || textChars.size() < wordChars.size())
    return std::nullopt;
  std::u32string out;
  bool found = false;
  for (std::size_t start = 0; start < textChars.size();) {
    auto end = start + wordChars.size();
    if (end > textChars.size()) {
      out.insert(out.end(), textChars.begin() + static_cast<std::ptrdiff_t>(start),
                 textChars.end());
      break;
    }
    bool boundary = (start == 0 || !isAlnum(textChars[start - 1])) &&
                    (end == textChars.size() || !isAlnum(textChars[end]));
    bool matched = boundary;
    if (matched) {
      for (std::size_t j = 0; j < wordChars.size(); ++j) {
        if (lowerCp(textChars[start + j]) != lowerCp(wordChars[j])) {
          matched = false;
          break;
        }
      }
    }
    if (matched) {
      out.append(U"______");
      found = true;
      start = end;
    } else {
      out.push_back(textChars[start]);
      ++start;
    }
  }
  if (!found)
    return std::nullopt;
  return fromU32(out);
}
std::vector<Json> parseJsonItems(const std::string &content) {
  auto text = trimText(content);
  if (text.compare(0, 7, "```json") == 0)
    text = text.substr(7);
  else if (text.compare(0, 3, "```") == 0)
    text = text.substr(3);
  text = trimText(text);
  if (text.size() >= 3 && text.compare(text.size() - 3, 3, "```") == 0)
    text.resize(text.size() - 3);
  text = trimText(text);
  auto open = text.find('[');
  auto close = text.rfind(']');
  if (open != std::string::npos && close != std::string::npos && close > open)
    text = text.substr(open, close - open + 1);
  try {
    auto parsed = Json::parse(text);
    if (!parsed.is_array())
      return {};
    return std::vector<Json>(parsed.begin(), parsed.end());
  } catch (const Json::exception &) {
    return {};
  }
}
std::vector<AiExample> validateAndBlank(const std::string &content,
                                        const std::vector<GenWord> &words) {
  std::vector<AiExample> out;
  for (const auto &item : parseJsonItems(content)) {
    if (!item.is_object() || !item.contains("word_id") ||
        !item["word_id"].is_number_integer() || !item.contains("sentence") ||
        !item["sentence"].is_string())
      continue;
    auto word = findWord(words, item["word_id"].get<std::int64_t>());
    if (!word)
      continue;
    auto sentence = trimText(item["sentence"].get<std::string>());
    if (sentence.empty())
      continue;
    auto chars = toU32(sentence);
    auto count = whitespaceWords(chars);
    if (chars.size() > 160 || count < 4 || count > 20)
      continue;
    if (item.contains("sense_id") && !item["sense_id"].is_null()) {
      if (!item["sense_id"].is_number_integer() ||
          word->sense_id != item["sense_id"].get<std::int64_t>())
        continue;
    }
    auto blanked = blankWord(sentence, word->word);
    if (!blanked)
      continue;
    out.push_back(AiExample{word->word_id, word->sense_id, *blanked});
  }
  return out;
}
} // namespace lexica::parity
