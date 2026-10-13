// 公共编号和数量限制：各模块共用这些类型与常量，避免规则不一致。
#pragma once
#include <cstdint>
#include <string>

// 给常用编号起别名；EntryId 是词条编号，QuestionId 是某次出题的编号。
using EntryId = std::int64_t;
using QuestionId = std::int64_t;
// SessionId 区分每轮复习；OpId 区分每次正式查词，重试时应复用它。
using SessionId = std::string;
using OpId = std::string;

// 笔记长度、分页数量、联想数量和每轮题数都在这里统一限制。
constexpr int kNoteMaxChars = 5000;
constexpr int kPageDefault = 20;
constexpr int kPageMax = 100;
constexpr int kSuggestMax = 8;
constexpr int kReviewFormalMax = 12;
