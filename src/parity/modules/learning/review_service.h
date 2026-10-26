#pragma once
#include "parity/contracts/ports.h"
#include "parity/modules/learning/grader.h"
namespace lexica::parity::learning {
StudySessionGroup studyGroup(sqlite3 *db, DictionaryPort &dictionary,
                             const std::string &wordbook, std::int64_t dailyNewLimit,
                             StudyPriority priority, UtcMillis now);
ReviewFeedback studyGrade(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                          StudyGrade grade, UtcMillis now);
std::vector<WordbookInfo> wordbooksList(sqlite3 *db);
void customWordbookToggle(sqlite3 *db, std::int64_t wordId, bool added);
std::vector<ReviewGroupEntry> reviewGroup(sqlite3 *db, DictionaryPort &dictionary,
                                          UtcMillis now);
std::vector<ReviewItem> reviewQueue(sqlite3 *db, DictionaryPort &dictionary,
                                    UtcMillis now);
bool practiceCheck(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                   const std::optional<std::string> &answer);
Hint practiceHint(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                  const std::optional<std::int64_t> &senseId, std::int64_t hintNo);
std::int64_t startReview(sqlite3 *db, DictionaryPort &dictionary, std::int64_t wordId,
                         const std::optional<std::int64_t> &senseId,
                         const std::optional<std::int64_t> &exampleId, UtcMillis now);
Hint requestHint(sqlite3 *db, DictionaryPort &dictionary, std::int64_t reviewId,
                 std::int64_t hintNo, UtcMillis now);
ReviewFeedback submitReview(sqlite3 *db, DictionaryPort &dictionary, std::int64_t reviewId,
                            const std::optional<std::string> &answer, UtcMillis now);
} // namespace lexica::parity::learning
