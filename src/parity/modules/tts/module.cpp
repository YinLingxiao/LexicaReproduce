#include "parity/modules/modules.h"
#include "parity/modules/tts/engine.h"
namespace lexica::parity {
void registerTts(Router &router) {
  router.bind("platform", "tts_voices",
              [](const Json &, Context &) -> Json { return ttsVoices(); });
  router.bind(
      "platform", "tts_synthesize",
      [](const Json &args, Context &context) -> Json {
        return ttsSynthesize(args, context.cancellation);
      });
  router.bind("platform", "tts_cancel",
              [](const Json &args, Context &) -> Json {
                const auto input = args.get<TtsCancelArgs>();
                ttsCancel(input.token, input.clientId.value_or("legacy"));
                return Json(nullptr);
              });
}
} // namespace lexica::parity
