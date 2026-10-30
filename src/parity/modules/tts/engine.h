#pragma once
#include "parity/contracts/cancel.h"
#include "parity/contracts/dto.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace lexica::parity {
inline std::string ttsLanguageAccent(std::string language) {
  for (char &c : language)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::replace(language.begin(), language.end(), '_', '-');
  if (language.rfind("en-gb", 0) == 0 || language.rfind("en-uk", 0) == 0)
    return "UK";
  if (language.rfind("en-us", 0) == 0)
    return "US";
  if (language == "en" || language.rfind("en-", 0) == 0)
    return "OTHER";
  return "";
}
struct TtsVoiceInfo {
  std::string id;
  std::string label;
  std::string language;
  std::string accent;
};
struct TtsCatalog {
  bool supported = false;
  std::string message;
  std::vector<TtsVoiceInfo> voices;
};
struct TtsAudio {
  std::string mime;
  std::vector<std::uint8_t> bytes;
};
class TtsEngine {
public:
  virtual ~TtsEngine() = default;
  virtual TtsCatalog catalog() = 0;
  virtual TtsAudio synthesize(const TtsVoiceInfo &voice, const std::string &text,
                              double rate, const std::function<bool()> &cancelled) = 0;
  virtual void cancel() = 0;
};
void setTtsEngine(std::shared_ptr<TtsEngine> engine);
void usePlatformTtsEngine();
Json ttsVoices();
Json ttsSynthesize(const Json &args, const Cancellation *cancellation);
void ttsCancel(std::int64_t token, const std::string &clientId = "legacy");
std::string ttsInstallMessage();
} // namespace lexica::parity
