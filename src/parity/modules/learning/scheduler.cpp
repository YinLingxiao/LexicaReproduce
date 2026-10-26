#include "parity/modules/learning/scheduler.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace lexica::parity::learning {
namespace {
struct Params {
  double retention = 0.90;
  double s0Context = 2.0;
  double s0English = 1.0;
  double s0Chinese = 0.5;
  double s0Unknown = 0.25;
  double gain[4] = {1.3, 1.7, 2.3, 3.0};
  double gainDifficultyBase = 1.1;
  double lapseFactor = 0.3;
  double lapseFloor = 0.25;
  double lapseDifficulty = 0.15;
  double contextVisitGain = 1.25;
  double weakVisitFactor = 0.85;
  double weakVisitDifficulty = 0.05;
  double successDifficultyRelief = 0.03;
  double difficultyMin = 0.05;
  double difficultyMax = 0.95;
  double difficulty0 = 0.3;
  double relearnHorizonFactor = 0.5;
  double relearnFloor = 0.25;
  std::uint32_t priorityWeakStreak = 2;
  std::int64_t priorityVisitWindowDays = 14;
  std::size_t priorityVisitCount = 3;
};
const Params kParams{};
struct Core {
  double strength_days = 0;
  double difficulty = 0;
  std::int64_t review_count = 0;
  std::int64_t lapse_count = 0;
  std::int64_t visit_count = 0;
  std::int64_t weak_streak = 0;
  UtcMillis first_visit_at = 0;
  std::optional<UtcMillis> last_visit_at;
  std::optional<UtcMillis> last_review_at;
  std::optional<UtcMillis> last_reinforcement_at;
  std::optional<UtcMillis> next_review_at;
};
bool isWeak(ComprehensionLevel level) {
  return level == ComprehensionLevel::Chinese ||
         level == ComprehensionLevel::Unknown;
}
double initialStrength(ComprehensionLevel level) {
  switch (level) {
  case ComprehensionLevel::Context:
    return kParams.s0Context;
  case ComprehensionLevel::English:
    return kParams.s0English;
  case ComprehensionLevel::Chinese:
    return kParams.s0Chinese;
  case ComprehensionLevel::Unknown:
    return kParams.s0Unknown;
  }
  return kParams.s0Unknown;
}
int qualityIndex(RecallQuality quality) {
  switch (quality) {
  case RecallQuality::Poor:
    return 1;
  case RecallQuality::Hard:
    return 2;
  case RecallQuality::Good:
    return 3;
  case RecallQuality::Excellent:
    return 4;
  case RecallQuality::Forgotten:
    return 0;
  }
  return 0;
}
UtcMillis addDays(UtcMillis at, double days) { return at + daysToMillis(days); }
void applyVisit(Core &core, UtcMillis at, ComprehensionLevel level) {
  core.visit_count += 1;
  core.last_visit_at = at;
  if (level == ComprehensionLevel::Context) {
    core.strength_days *= kParams.contextVisitGain;
    core.next_review_at = addDays(at, core.strength_days);
    core.last_reinforcement_at = at;
    core.weak_streak = 0;
    return;
  }
  if (level == ComprehensionLevel::English)
    return;
  core.strength_days =
      std::max(core.strength_days * kParams.weakVisitFactor, kParams.lapseFloor);
  core.difficulty = std::min(core.difficulty + kParams.weakVisitDifficulty,
                             kParams.difficultyMax);
  core.weak_streak += 1;
  const auto horizon = std::max(core.strength_days * kParams.relearnHorizonFactor,
                                kParams.relearnFloor);
  const auto candidate = addDays(at, horizon);
  core.next_review_at =
      core.next_review_at ? std::min(*core.next_review_at, candidate) : candidate;
}
void applyReview(Core &core, UtcMillis at, RecallQuality quality) {
  core.review_count += 1;
  core.last_review_at = at;
  core.last_reinforcement_at = at;
  core.weak_streak = 0;
  if (quality == RecallQuality::Forgotten) {
    core.strength_days =
        std::max(core.strength_days * kParams.lapseFactor, kParams.lapseFloor);
    core.difficulty = std::min(core.difficulty + kParams.lapseDifficulty,
                               kParams.difficultyMax);
    core.lapse_count += 1;
  } else {
    const auto index = qualityIndex(quality);
    const auto gain =
        kParams.gain[index - 1] * (kParams.gainDifficultyBase - core.difficulty);
    core.strength_days = std::max(core.strength_days * gain, kParams.lapseFloor);
    core.difficulty =
        std::max(core.difficulty - kParams.successDifficultyRelief *
                                       (static_cast<double>(index) - 1.0),
                 kParams.difficultyMin);
  }
  core.next_review_at = addDays(at, core.strength_days);
}
bool visitsInWindow(const std::vector<UtcMillis> &times, std::int64_t window,
                    std::size_t need) {
  if (times.size() < need || need == 0)
    return false;
  for (std::size_t i = 0; i < times.size(); ++i) {
    std::size_t count = 1;
    for (std::size_t j = i + 1; j < times.size(); ++j) {
      if (times[j] - times[i] <= window) {
        if (++count >= need)
          return true;
      } else
        break;
    }
  }
  return false;
}
Projection finish(std::int64_t wordId, const Core &core, bool highPriority) {
  Projection state;
  state.word_id = wordId;
  state.strength_days = core.strength_days;
  state.difficulty = core.difficulty;
  state.review_count = core.review_count;
  state.lapse_count = core.lapse_count;
  state.visit_count = core.visit_count;
  state.weak_streak = core.weak_streak;
  state.high_priority = highPriority;
  state.first_visit_at = core.first_visit_at;
  state.last_visit_at = core.last_visit_at;
  state.last_review_at = core.last_review_at;
  state.last_reinforcement_at = core.last_reinforcement_at;
  state.next_review_at = core.next_review_at;
  return state;
}
} // namespace
std::int64_t daysToMillis(double days) {
  return static_cast<std::int64_t>(std::llround(days * static_cast<double>(kDayMs)));
}
MemoryStatus statusOf(double strengthDays) {
  if (strengthDays < kFamiliarDays)
    return MemoryStatus::Learning;
  if (strengthDays < kStableDays)
    return MemoryStatus::Familiar;
  return MemoryStatus::Stable;
}
const char *levelName(ComprehensionLevel level) {
  switch (level) {
  case ComprehensionLevel::Context:
    return "context";
  case ComprehensionLevel::English:
    return "english";
  case ComprehensionLevel::Chinese:
    return "chinese";
  case ComprehensionLevel::Unknown:
    return "unknown";
  }
  return "unknown";
}
const char *qualityName(RecallQuality quality) {
  switch (quality) {
  case RecallQuality::Excellent:
    return "excellent";
  case RecallQuality::Good:
    return "good";
  case RecallQuality::Hard:
    return "hard";
  case RecallQuality::Poor:
    return "poor";
  case RecallQuality::Forgotten:
    return "forgotten";
  }
  return "forgotten";
}
std::optional<ComprehensionLevel> parseLevel(const std::string &value) {
  if (value == "context")
    return ComprehensionLevel::Context;
  if (value == "english")
    return ComprehensionLevel::English;
  if (value == "chinese")
    return ComprehensionLevel::Chinese;
  if (value == "unknown")
    return ComprehensionLevel::Unknown;
  return std::nullopt;
}
std::optional<RecallQuality> parseQuality(const std::string &value) {
  if (value == "excellent")
    return RecallQuality::Excellent;
  if (value == "good")
    return RecallQuality::Good;
  if (value == "hard")
    return RecallQuality::Hard;
  if (value == "poor")
    return RecallQuality::Poor;
  if (value == "forgotten")
    return RecallQuality::Forgotten;
  return std::nullopt;
}
int helpRank(ComprehensionLevel level) {
  if (level == ComprehensionLevel::Context)
    return 0;
  if (level == ComprehensionLevel::English)
    return 1;
  return 2;
}
std::optional<Projection> fold(std::int64_t wordId,
                               const std::vector<MemoryEvent> &events) {
  std::optional<Core> core;
  std::vector<UtcMillis> priorityVisits;
  for (const auto &event : events) {
    if (!event.review) {
      if (event.level == ComprehensionLevel::Context)
        priorityVisits.clear();
      else
        priorityVisits.push_back(event.at);
      if (!core) {
        const auto strength = initialStrength(event.level);
        Core created;
        created.strength_days = strength;
        created.difficulty = kParams.difficulty0;
        created.visit_count = 1;
        created.weak_streak = isWeak(event.level) ? 1 : 0;
        created.first_visit_at = event.at;
        created.last_visit_at = event.at;
        created.last_reinforcement_at = event.at;
        created.next_review_at = addDays(event.at, strength);
        core = created;
      } else
        applyVisit(*core, event.at, event.level);
    } else {
      if (event.quality != RecallQuality::Forgotten)
        priorityVisits.clear();
      if (!core) {
        Core created;
        created.strength_days = kParams.s0Unknown;
        created.difficulty = kParams.difficulty0;
        created.first_visit_at = event.at;
        created.last_reinforcement_at = event.at;
        created.next_review_at = addDays(event.at, kParams.s0Unknown);
        applyReview(created, event.at, event.quality);
        core = created;
      } else
        applyReview(*core, event.at, event.quality);
    }
  }
  if (!core)
    return std::nullopt;
  const auto window = kParams.priorityVisitWindowDays * kDayMs;
  const auto high = core->weak_streak >=
                        static_cast<std::int64_t>(kParams.priorityWeakStreak) ||
                    visitsInWindow(priorityVisits, window, kParams.priorityVisitCount);
  return finish(wordId, *core, high);
}
double retrievability(const Projection &state, UtcMillis now) {
  if (!state.last_reinforcement_at)
    return 0;
  const auto elapsed =
      static_cast<double>(now - *state.last_reinforcement_at) /
      static_cast<double>(kDayMs);
  if (elapsed <= 0)
    return 1;
  const auto strength =
      std::max(state.strength_days, std::numeric_limits<double>::min());
  return std::pow(kParams.retention, elapsed / strength);
}
MemoryState toDto(const Projection &state) {
  MemoryState dto;
  dto.word_id = state.word_id;
  dto.strength_days = state.strength_days;
  dto.difficulty = state.difficulty;
  dto.review_count = state.review_count;
  dto.lapse_count = state.lapse_count;
  dto.visit_count = state.visit_count;
  dto.weak_streak = state.weak_streak;
  dto.high_priority = state.high_priority;
  dto.first_visit_at = formatUtc(state.first_visit_at);
  if (state.last_visit_at)
    dto.last_visit_at = formatUtc(*state.last_visit_at);
  if (state.last_review_at)
    dto.last_review_at = formatUtc(*state.last_review_at);
  if (state.last_reinforcement_at)
    dto.last_reinforcement_at = formatUtc(*state.last_reinforcement_at);
  if (state.next_review_at)
    dto.next_review_at = formatUtc(*state.next_review_at);
  return dto;
}
} // namespace lexica::parity::learning
