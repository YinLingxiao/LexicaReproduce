#include "parity/modules/dictionary/roots.h"
#include "parity/modules/dictionary/normalize.h"
#include "roots_data.inc"
#include <algorithm>
#include <iterator>
#include <unordered_map>
namespace lexica::parity {
namespace {
std::vector<char32_t> codePoints(const std::string &text) {
  std::vector<char32_t> out;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    char32_t cp = lead;
    std::size_t length = 1;
    if ((lead & 0x80) == 0) {
      cp = lead;
    } else if ((lead & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp = (lead & 0x1F) << 6 |
           (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      length = 2;
    } else if ((lead & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp = (lead & 0x0F) << 12 |
           (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6 |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      length = 3;
    } else if ((lead & 0xF8) == 0xF0 && i + 3 < text.size()) {
      cp = (lead & 0x07) << 18 |
           (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12 |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6 |
           (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      length = 4;
    }
    out.push_back(cp);
    i += length;
  }
  return out;
}
std::vector<std::string> whitespaceTokens(const std::string &text) {
  std::vector<std::string> tokens;
  std::string current;
  auto flush = [&] {
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  };
  const auto points = codePoints(text);
  std::size_t byte = 0;
  for (char32_t cp : points) {
    std::size_t length = 1;
    if (cp >= 0x80 && cp < 0x800)
      length = 2;
    else if (cp >= 0x800 && cp < 0x10000)
      length = 3;
    else if (cp >= 0x10000)
      length = 4;
    const bool space = (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 ||
                       cp == 0x0085 || cp == 0x00A0 || cp == 0x1680 ||
                       (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
                       cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
                       cp == 0x3000;
    if (space)
      flush();
    else
      current.append(text, byte, length);
    byte += length;
  }
  flush();
  return tokens;
}
} // namespace
std::size_t affixRuleCount() {
  return std::size(kPrefixRules) + std::size(kRootRules) + std::size(kSuffixRules);
}
std::vector<RootPart> decomposeRoots(const std::string &word) {
  std::string w = trimCopy(word);
  for (char &ch : w)
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
  if (w.empty())
    return {};
  std::vector<RootPart> parts;
  std::size_t start = 0;
  std::size_t end = w.size();
  for (const auto &rule : kPrefixRules) {
    const std::string prefix = rule.form;
    if (w.size() >= prefix.size() + 3 &&
        w.compare(0, prefix.size(), prefix) == 0) {
      parts.push_back({prefix + "-", rule.meaning});
      start = prefix.size();
      break;
    }
  }
  std::optional<RootPart> suffix;
  const auto remaining = w.size() - start;
  for (const auto &rule : kSuffixRules) {
    const std::string form = rule.form;
    if (remaining >= form.size() + 2 && endsWith(w, form)) {
      suffix = RootPart{"-" + form, rule.meaning};
      end = w.size() - form.size();
      break;
    }
  }
  if (end < start)
    end = start;
  const std::string stem = w.substr(start, end - start);
  bool found = false;
  for (const auto &rule : kRootRules) {
    if (stem.find(rule.form) != std::string::npos) {
      parts.push_back({rule.form, rule.meaning});
      found = true;
      break;
    }
  }
  if (!found) {
    for (const auto &rule : kRootRules) {
      if (w.find(rule.form) != std::string::npos) {
        parts.push_back({rule.form, rule.meaning});
        found = true;
        break;
      }
    }
  }
  if (!found && !stem.empty() && (parts.empty() || suffix))
    parts.push_back({stem, "核心词干"});
  if (suffix)
    parts.push_back(*suffix);
  return parts;
}
std::size_t levenshtein(const std::string &left, const std::string &right) {
  if (left == right)
    return 0;
  const auto a = codePoints(left);
  const auto b = codePoints(right);
  if (a.empty())
    return b.size();
  if (b.empty())
    return a.size();
  std::vector<std::size_t> prev(b.size() + 1);
  std::vector<std::size_t> curr(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j)
    prev[j] = j;
  for (std::size_t i = 0; i < a.size(); ++i) {
    curr[0] = i + 1;
    for (std::size_t j = 0; j < b.size(); ++j) {
      const std::size_t cost = a[i] == b[j] ? 0 : 1;
      curr[j + 1] = std::min(prev[j + 1] + 1, std::min(curr[j] + 1, prev[j] + cost));
    }
    prev.swap(curr);
  }
  return prev[b.size()];
}
bool isPosToken(const std::string &token) {
  return token == "n." || token == "v." || token == "vt." || token == "vi." ||
         token == "adj." || token == "adv." || token == "prep." ||
         token == "conj." || token == "pron." || token == "art." ||
         token == "num." || token == "int." || token == "a." || token == "s." ||
         token == "noun" || token == "verb" || token == "adjective" ||
         token == "adverb";
}
std::tuple<std::string, std::optional<std::string>, std::optional<std::string>>
parseWordFamilyEntry(const std::string &raw) {
  const auto trimmed = trimCopy(raw);
  if (trimmed.empty())
    return {{}, std::nullopt, std::nullopt};
  const auto tokens = whitespaceTokens(trimmed);
  if (tokens.empty())
    return {{}, std::nullopt, std::nullopt};
  const auto &word = tokens[0];
  const auto rest = trimCopy(trimmed.substr(word.size()));
  if (rest.empty())
    return {word, std::nullopt, std::nullopt};
  if (tokens.size() >= 2 && isPosToken(tokens[1])) {
    const auto after = trimCopy(rest.substr(tokens[1].size()));
    return {word, tokens[1],
            after.empty() ? std::nullopt : std::optional<std::string>(after)};
  }
  return {word, std::nullopt, rest};
}
std::pair<std::optional<std::string>, std::string>
extractPosAndCleanGloss(const std::optional<std::string> &pos,
                        const std::string *chinese, const std::string *english) {
  const std::string *chosen = nullptr;
  if (chinese && !trimCopy(*chinese).empty())
    chosen = chinese;
  else if (english && !trimCopy(*english).empty())
    chosen = english;
  std::string raw;
  if (chosen) {
    const auto lineEnd = chosen->find_first_of("\r\n");
    raw = trimCopy(chosen->substr(0, lineEnd));
  }
  auto finalPos = pos && !trimCopy(*pos).empty() ? pos : std::nullopt;
  auto gloss = raw;
  const char *prefixes[] = {"n.",  "vt.", "vi.",  "v.",   "adj.", "adv.",
                            "prep.", "conj.", "pron.", "art.", "num.", "int.",
                            "a."};
  for (const char *prefix : prefixes) {
    const std::string key = prefix;
    if (raw.rfind(key, 0) != 0)
      continue;
    const auto rest = raw.substr(key.size());
    if (!rest.empty() && !(rest[0] == ' ' || rest[0] == '\t'))
      continue;
    if (!finalPos)
      finalPos = key == "a." ? std::optional<std::string>("adj.")
                             : std::optional<std::string>(key);
    const auto stripped = trimCopy(rest);
    if (!stripped.empty())
      gloss = stripped;
    break;
  }
  return {finalPos, gloss};
}
const FallbackGloss *fallbackRelatedGloss(const std::string &word) {
  static const auto index = [] {
    std::unordered_map<std::string, FallbackGloss> map;
    map.reserve(std::size(kFallbackGloss));
    for (const auto &item : kFallbackGloss)
      map.emplace(item.word, FallbackGloss{item.pos, item.gloss});
    return map;
  }();
  const auto found = index.find(word);
  if (found == index.end())
    return nullptr;
  return &found->second;
}
} // namespace lexica::parity
