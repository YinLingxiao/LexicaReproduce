#pragma once
#include <cstdint>
#include <string>

using EntryId = std::int64_t;
using QuestionId = std::int64_t;
using SessionId = std::string;
using OpId = std::string;

constexpr int kNoteMaxChars = 5000;
constexpr int kPageDefault = 20;
constexpr int kPageMax = 100;
constexpr int kSuggestMax = 8;
constexpr int kReviewFormalMax = 12;
