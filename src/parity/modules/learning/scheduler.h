#pragma once
#include "parity/contracts/clock.h"
#include "parity/contracts/dto.h"
#include <optional>
#include <vector>
namespace lexica::parity::learning {
inline constexpr double kFamiliarDays = 4.0;
inline constexpr double kStableDays = 21.0;
inline constexpr std::int64_t kVisitMergeMs = 30 * 60 * 1000;
inline constexpr std::int64_t kQueueActiveMs = 60LL * 86400000LL;
inline constexpr int kReviewLimit = 10;
inline constexpr int kStudyLimit = 20;
inline constexpr std::int64_t kDayMs = 86400000LL;

std::int64_t daysToMillis(double days);
MemoryStatus statusOf(double strengthDays);
const char *levelName(ComprehensionLevel level);
const char *qualityName(RecallQuality quality);
std::optional<ComprehensionLevel> parseLevel(const std::string &value);
std::optional<RecallQuality> parseQuality(const std::string &value);
int helpRank(ComprehensionLevel level);

struct MemoryEvent {
  bool review = false;
  UtcMillis at = 0;
  ComprehensionLevel level = ComprehensionLevel::Unknown;
  RecallQuality quality = RecallQuality::Forgotten;
};
struct Projection {
  std::int64_t word_id = 0;
  double strength_days = 0;
  double difficulty = 0;
  std::int64_t review_count = 0;
  std::int64_t lapse_count = 0;
  std::int64_t visit_count = 0;
  std::int64_t weak_streak = 0;
  bool high_priority = false;
  UtcMillis first_visit_at = 0;
  std::optional<UtcMillis> last_visit_at;
  std::optional<UtcMillis> last_review_at;
  std::optional<UtcMillis> last_reinforcement_at;
  std::optional<UtcMillis> next_review_at;
};
std::optional<Projection> fold(std::int64_t wordId,
                               const std::vector<MemoryEvent> &events);
double retrievability(const Projection &state, UtcMillis now);
MemoryState toDto(const Projection &state);
} // namespace lexica::parity::learning
