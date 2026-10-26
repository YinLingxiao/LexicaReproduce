#include "parity/modules/learning/memory_service.h"
#include "parity/modules/learning/review_service.h"
#include "parity/modules/modules.h"
#include "parity/storage/database.h"
#include <map>
#include <mutex>
namespace lexica::parity {
namespace {
struct Receipt {
  Json args;
  Json result;
};
std::mutex receiptsMutex;
std::map<std::string, Receipt> receipts;
std::optional<Json> replay(const std::string &command, const std::string &requestId,
                           const Json &args) {
  if (requestId.empty())
    return std::nullopt;
  std::lock_guard<std::mutex> lock(receiptsMutex);
  const auto found = receipts.find(command + "\n" + requestId);
  if (found == receipts.end())
    return std::nullopt;
  if (found->second.args != args)
    throw Error("BAD_REQUEST", "request_id reused with different arguments");
  return found->second.result;
}
void remember(const std::string &command, const std::string &requestId, const Json &args,
              const Json &result) {
  if (requestId.empty())
    return;
  std::lock_guard<std::mutex> lock(receiptsMutex);
  receipts.insert_or_assign(command + "\n" + requestId, Receipt{args, result});
}
} // namespace
void registerLearning(Router &router) {
  router.bind(
      "learning", "lookup_word",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<LookupWordArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("lookup_word", context.request_id, args))
            return *prior;
          LookupWordResult result;
          if (auto entry = context.dictionary.find(db, input.word)) {
            Transaction tx(db);
            const auto now = context.clock.nowMillis();
            const auto id = learning::openVisit(db, entry->id, now);
            const auto state = learning::loadState(db, entry->id);
            const auto encounter = learning::getEncounter(db, id);
            tx.commit();
            result.outcome = LookupOutcomeHit{std::move(*entry)};
            result.encounter_id = id;
            if (state) {
              MemorySummary summary;
              summary.status = learning::statusOf(state->strength_days);
              summary.visit_count = state->visit_count;
              summary.high_priority = state->high_priority;
              result.memory = summary;
            }
            if (encounter)
              result.comprehension_level = encounter->level;
          } else
            result.outcome =
                LookupOutcomeMiss{context.dictionary.suggest(db, input.word, 8)};
          Json out = result;
          remember("lookup_word", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "set_comprehension",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<SetComprehensionArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("set_comprehension", context.request_id, args))
            return *prior;
          Transaction tx(db);
          learning::setComprehension(db, input.encounterId, input.level,
                                     context.clock.nowMillis());
          tx.commit();
          Json out = nullptr;
          remember("set_comprehension", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "rebuild_memory_projection",
      [](const Json &args, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("rebuild_memory_projection", context.request_id, args))
            return *prior;
          Json out = learning::rebuildProjection(db, context.clock.nowMillis());
          remember("rebuild_memory_projection", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "study_group",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<StudyGroupArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::studyGroup(db, context.dictionary, input.wordbook,
                                      input.dailyNewLimit, input.priority,
                                      context.clock.nowMillis());
        });
      },
      "implemented");
  router.bind(
      "learning", "study_grade",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<StudyGradeArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("study_grade", context.request_id, args))
            return *prior;
          Json out = learning::studyGrade(db, context.dictionary, input.wordId,
                                          input.grade, context.clock.nowMillis());
          remember("study_grade", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "wordbooks_list",
      [](const Json &, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::wordbooksList(db);
        });
      },
      "implemented");
  router.bind(
      "learning", "custom_wordbook_toggle",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<CustomWordbookToggleArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("custom_wordbook_toggle", context.request_id, args))
            return *prior;
          learning::customWordbookToggle(db, input.wordId, input.added);
          Json out = nullptr;
          remember("custom_wordbook_toggle", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "review_queue",
      [](const Json &, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::reviewQueue(db, context.dictionary, context.clock.nowMillis());
        });
      },
      "implemented");
  router.bind(
      "learning", "review_group",
      [](const Json &, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::reviewGroup(db, context.dictionary, context.clock.nowMillis());
        });
      },
      "implemented");
  router.bind(
      "learning", "practice_check",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<PracticeCheckArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::practiceCheck(db, context.dictionary, input.wordId, input.answer);
        });
      },
      "implemented");
  router.bind(
      "learning", "practice_hint",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<PracticeHintArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::practiceHint(db, context.dictionary, input.wordId, input.senseId,
                                        input.hintNo);
        });
      },
      "implemented");
  router.bind(
      "learning", "start_review_item",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<StartReviewItemArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("start_review_item", context.request_id, args))
            return *prior;
          Json out = learning::startReview(db, context.dictionary, input.wordId, input.senseId,
                                           input.exampleId, context.clock.nowMillis());
          remember("start_review_item", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "request_hint",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<RequestHintArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("request_hint", context.request_id, args))
            return *prior;
          Json out = learning::requestHint(db, context.dictionary, input.reviewId, input.hintNo,
                                           context.clock.nowMillis());
          remember("request_hint", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "submit_review",
      [](const Json &args, Context &context) -> Json {
        const auto input = args.get<SubmitReviewArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = replay("submit_review", context.request_id, args))
            return *prior;
          Json out = learning::submitReview(db, context.dictionary, input.reviewId, input.answer,
                                            context.clock.nowMillis());
          remember("submit_review", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
}
} // namespace lexica::parity
