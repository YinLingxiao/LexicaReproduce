#include "parity/modules/learning/memory_service.h"
#include "parity/modules/learning/review_service.h"
#include "parity/modules/modules.h"
#include "parity/storage/database.h"
#include <deque>
#include <map>
#include <memory>
#include <mutex>
namespace lexica::parity {
namespace {
struct Receipt {
  Json args;
  Json result;
};
// One receipt store per registered Application, never shared across databases.
// Bound the in-memory retry window so normal long-running use cannot leak RAM.
class ReceiptStore {
  std::mutex mutex_;
  std::map<std::string, Receipt> entries_;
  std::deque<std::string> order_;
  std::size_t bytes_ = 0;

public:
  std::optional<Json> replay(const std::string &command, const std::string &id,
                             const Json &args) {
    if (id.empty())
      return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(command + "\n" + id);
    if (found == entries_.end())
      return std::nullopt;
    if (found->second.args != args)
      throw Error("BAD_REQUEST", "request_id reused with different arguments");
    return found->second.result;
  }
  void remember(const std::string &command, const std::string &id,
                const Json &args, const Json &result) {
    if (id.empty())
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = command + "\n" + id;
    if (entries_.count(key))
      return;
    const auto size = args.dump().size() + result.dump().size();
    if (size > 16 * 1024 * 1024)
      return;
    while (!order_.empty() &&
           (entries_.size() >= 4096 || bytes_ + size > 16 * 1024 * 1024)) {
      const auto &old = entries_.at(order_.front());
      bytes_ -= old.args.dump().size() + old.result.dump().size();
      entries_.erase(order_.front());
      order_.pop_front();
    }
    entries_.emplace(key, Receipt{args, result});
    order_.push_back(key);
    bytes_ += size;
  }
};
} // namespace
void registerLearning(Router &router) {
  const auto receipts = std::make_shared<ReceiptStore>();
  router.bind(
      "learning", "lookup_word",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<LookupWordArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior =
                  receipts->replay("lookup_word", context.request_id, args))
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
            result.outcome = LookupOutcomeMiss{
                context.dictionary.suggest(db, input.word, 8)};
          Json out = result;
          receipts->remember("lookup_word", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "set_comprehension",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<SetComprehensionArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = receipts->replay("set_comprehension",
                                            context.request_id, args))
            return *prior;
          Transaction tx(db);
          learning::setComprehension(db, input.encounterId, input.level,
                                     context.clock.nowMillis());
          tx.commit();
          Json out = nullptr;
          receipts->remember("set_comprehension", context.request_id, args,
                             out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "rebuild_memory_projection",
      [receipts](const Json &args, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = receipts->replay("rebuild_memory_projection",
                                            context.request_id, args))
            return *prior;
          Json out = learning::rebuildProjection(db, context.clock.nowMillis());
          receipts->remember("rebuild_memory_projection", context.request_id,
                             args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "study_group",
      [receipts](const Json &args, Context &context) -> Json {
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
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<StudyGradeArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior =
                  receipts->replay("study_grade", context.request_id, args))
            return *prior;
          Json out =
              learning::studyGrade(db, context.dictionary, input.wordId,
                                   input.grade, context.clock.nowMillis());
          receipts->remember("study_grade", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "wordbooks_list",
      [receipts](const Json &, Context &context) -> Json {
        return context.db.withConnection(
            [&](sqlite3 *db) -> Json { return learning::wordbooksList(db); });
      },
      "implemented");
  router.bind(
      "learning", "custom_wordbook_toggle",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<CustomWordbookToggleArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = receipts->replay("custom_wordbook_toggle",
                                            context.request_id, args))
            return *prior;
          learning::customWordbookToggle(db, input.wordId, input.added);
          Json out = nullptr;
          receipts->remember("custom_wordbook_toggle", context.request_id, args,
                             out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "review_queue",
      [receipts](const Json &, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::reviewQueue(db, context.dictionary,
                                       context.clock.nowMillis());
        });
      },
      "implemented");
  router.bind(
      "learning", "review_group",
      [receipts](const Json &, Context &context) -> Json {
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::reviewGroup(db, context.dictionary,
                                       context.clock.nowMillis());
        });
      },
      "implemented");
  router.bind(
      "learning", "practice_check",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<PracticeCheckArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::practiceCheck(db, context.dictionary, input.wordId,
                                         input.answer);
        });
      },
      "implemented");
  router.bind(
      "learning", "practice_hint",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<PracticeHintArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          return learning::practiceHint(db, context.dictionary, input.wordId,
                                        input.senseId, input.hintNo);
        });
      },
      "implemented");
  router.bind(
      "learning", "start_review_item",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<StartReviewItemArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior = receipts->replay("start_review_item",
                                            context.request_id, args))
            return *prior;
          Json out = learning::startReview(db, context.dictionary, input.wordId,
                                           input.senseId, input.exampleId,
                                           context.clock.nowMillis());
          receipts->remember("start_review_item", context.request_id, args,
                             out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "request_hint",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<RequestHintArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior =
                  receipts->replay("request_hint", context.request_id, args))
            return *prior;
          Json out =
              learning::requestHint(db, context.dictionary, input.reviewId,
                                    input.hintNo, context.clock.nowMillis());
          receipts->remember("request_hint", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
  router.bind(
      "learning", "submit_review",
      [receipts](const Json &args, Context &context) -> Json {
        const auto input = args.get<SubmitReviewArgs>();
        return context.db.withConnection([&](sqlite3 *db) -> Json {
          if (auto prior =
                  receipts->replay("submit_review", context.request_id, args))
            return *prior;
          Json out =
              learning::submitReview(db, context.dictionary, input.reviewId,
                                     input.answer, context.clock.nowMillis());
          receipts->remember("submit_review", context.request_id, args, out);
          return out;
        });
      },
      "implemented");
}
} // namespace lexica::parity
