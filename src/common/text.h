// 公共文本工具：统一单词写法，并判断文本中是否出现完整单词。
#pragma once
#include <cctype>
#include <string>

// 去掉两端空白并转为小写，让查词和判分使用统一格式。
inline std::string normalizeWord(const std::string& in) {
    std::size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) ++begin;
    std::size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) --end;
    std::string out;
    out.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// 按字母边界匹配整个单词，避免把 apple 当成 pineapple 的命中。
inline bool containsWholeWord(const std::string& text, const std::string& word) {
    std::string needle = normalizeWord(word);
    if (needle.empty()) return false;
    std::string lower;
    lower.reserve(text.size());
    for (unsigned char c : text) lower.push_back(static_cast<char>(std::tolower(c)));
    for (std::size_t i = 0; i + needle.size() <= lower.size(); ++i) {
        if (lower.compare(i, needle.size(), needle) != 0) continue;
        bool left = i == 0 || !std::isalpha(static_cast<unsigned char>(lower[i - 1]));
        std::size_t j = i + needle.size();
        bool right = j >= lower.size() || !std::isalpha(static_cast<unsigned char>(lower[j]));
        if (left && right) return true;
    }
    return false;
}
