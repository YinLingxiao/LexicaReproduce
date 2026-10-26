#include "parity/modules/dictionary/normalize.h"
#include "parity/contracts/error.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace lexica::parity {
namespace {
bool unicodeSpace(char32_t cp) {
  return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 || cp == 0x0085 ||
         cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
         cp == 0x3000;
}
std::wstring toWide(std::string_view text) {
  if (text.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
  if (n <= 0)
    throw Error("BAD_REQUEST", "Dictionary text is not valid UTF-8");
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), out.data(), n);
  return out;
}
std::string toUtf8(std::wstring_view text) {
  if (text.empty())
    return {};
  const int n =
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    throw Error("INTERNAL_ERROR", "Cannot encode dictionary text");
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}
std::wstring nfkc(const std::wstring &text) {
  if (text.empty())
    return {};
  int need = NormalizeString(NormalizationKC, text.c_str(),
                             static_cast<int>(text.size()), nullptr, 0);
  if (need < 0)
    need = -need;
  if (need <= 0)
    throw Error("INTERNAL_ERROR", "NFKC normalization failed");
  std::wstring out(static_cast<std::size_t>(need), L'\0');
  for (;;) {
    const int wrote =
        NormalizeString(NormalizationKC, text.c_str(),
                        static_cast<int>(text.size()), out.data(), need);
    if (wrote > 0) {
      out.resize(static_cast<std::size_t>(wrote));
      return out;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      throw Error("INTERNAL_ERROR", "NFKC normalization failed");
    need = wrote < 0 ? -wrote : need * 2;
    out.resize(static_cast<std::size_t>(need));
  }
}
std::wstring lower(const std::wstring &text) {
  if (text.empty())
    return {};
  const int need = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.c_str(),
      static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);
  if (need <= 0)
    throw Error("INTERNAL_ERROR", "Unicode lowercase failed");
  std::wstring out(static_cast<std::size_t>(need), L'\0');
  const int wrote = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.c_str(),
      static_cast<int>(text.size()), out.data(), need, nullptr, nullptr, 0);
  if (wrote <= 0)
    throw Error("INTERNAL_ERROR", "Unicode lowercase failed");
  out.resize(static_cast<std::size_t>(wrote));
  return out;
}
void trimWide(std::wstring &text) {
  std::size_t begin = 0;
  while (begin < text.size() && unicodeSpace(text[begin]))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && unicodeSpace(text[end - 1]))
    --end;
  text = text.substr(begin, end - begin);
}
void replaceQuotes(std::wstring &text) {
  for (auto &ch : text) {
    if (ch == 0x2018 || ch == 0x2019 || ch == L'`' || ch == 0x00B4)
      ch = L'\'';
    else if (ch == 0x201C || ch == 0x201D)
      ch = L'"';
  }
}
void appendUtf8(std::string &out, char32_t cp) {
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
bool lastCodePoint(std::string_view text, char32_t &cp, std::size_t &length) {
  if (text.empty())
    return false;
  std::size_t i = text.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
    --i;
  const auto lead = static_cast<unsigned char>(text[i]);
  length = text.size() - i;
  if (lead < 0x80 && length == 1) {
    cp = lead;
    return true;
  }
  if ((lead & 0xE0) == 0xC0 && length == 2) {
    cp = (lead & 0x1F) << 6 |
         (static_cast<unsigned char>(text[i + 1]) & 0x3F);
    return cp >= 0x80;
  }
  if ((lead & 0xF0) == 0xE0 && length == 3) {
    cp = (lead & 0x0F) << 12 |
         (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6 |
         (static_cast<unsigned char>(text[i + 2]) & 0x3F);
    return cp >= 0x800;
  }
  if ((lead & 0xF8) == 0xF0 && length == 4) {
    cp = (lead & 0x07) << 18 |
         (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12 |
         (static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6 |
         (static_cast<unsigned char>(text[i + 3]) & 0x3F);
    return cp >= 0x10000 && cp <= 0x10FFFF;
  }
  return false;
}
} // namespace
std::string trimCopy(std::string_view input) {
  auto wide = toWide(input);
  trimWide(wide);
  return toUtf8(wide);
}
std::string normalizeWord(std::string_view input) {
  auto wide = nfkc(toWide(input));
  trimWide(wide);
  wide = lower(wide);
  replaceQuotes(wide);
  auto text = toUtf8(wide);
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == '"' || text[begin] == '\''))
    ++begin;
  while (end > begin && (text[end - 1] == '"' || text[end - 1] == '\''))
    --end;
  return text.substr(begin, end - begin);
}
std::string prefixUpperBound(std::string_view prefix) {
  std::string bound(prefix);
  while (!bound.empty()) {
    char32_t cp = 0;
    std::size_t length = 0;
    if (!lastCodePoint(bound, cp, length)) {
      bound.pop_back();
      continue;
    }
    bound.resize(bound.size() - length);
    if (cp < 0x10FFFF) {
      const char32_t next = cp + 1;
      if (next < 0xD800 || next > 0xDFFF) {
        appendUtf8(bound, next);
        return bound;
      }
    }
  }
  appendUtf8(bound, 0x10FFFF);
  return bound;
}
std::string ftsQuoteTerm(std::string_view term) {
  std::string out = "\"";
  for (const char ch : term) {
    if (ch == '"')
      out += "\"\"";
    else
      out.push_back(ch);
  }
  out.push_back('"');
  return out;
}
std::size_t utf8Length(std::string_view text) {
  std::size_t count = 0;
  for (unsigned char ch : text)
    if ((ch & 0xC0) != 0x80)
      ++count;
  return count;
}
bool endsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace lexica::parity
