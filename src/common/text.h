#pragma once
#include <cctype>
#include <string>

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
