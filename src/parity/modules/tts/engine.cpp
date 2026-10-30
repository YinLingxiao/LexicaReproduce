#include "parity/modules/tts/engine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <atomic>
#include <unordered_map>
namespace lexica::parity {
std::shared_ptr<TtsEngine> createPlatformEngine();
namespace {
constexpr std::size_t kMaxAudio = 768 * 1024;
constexpr std::size_t kMaxText = 2000;

std::mutex engineMu;
std::shared_ptr<TtsEngine> slot;
std::mutex synthMu;
std::mutex epochMu;
struct ClientEpoch {
  std::shared_ptr<std::atomic<std::int64_t>> value =
      std::make_shared<std::atomic<std::int64_t>>(0);
  std::uint64_t touched = 0;
};
std::unordered_map<std::string, ClientEpoch> epochs;
std::uint64_t touchSequence = 0;
std::shared_ptr<std::atomic<std::int64_t>> activeEpoch;
bool running = false;
std::int64_t runningToken = 0;
std::shared_ptr<TtsEngine> activeEngine;

std::string fold(std::string value) {
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::replace(value.begin(), value.end(), '_', '-');
  return value;
}

std::string trim(std::string value) {
  auto space = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };
  while (!value.empty() && space(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && space(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

std::string base64Encode(const std::vector<std::uint8_t> &bytes) {
  static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const unsigned b0 = bytes[i];
    const unsigned b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const unsigned b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const unsigned n = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? table[n & 63] : '=');
  }
  return out;
}

bool allowedMime(const std::string &mime) {
  return mime == "audio/wav" || mime == "audio/x-wav" || mime == "audio/wave" ||
         mime == "audio/mpeg" || mime == "audio/mp3";
}

bool looksLikeWav(const std::vector<std::uint8_t> &bytes) {
  return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
         std::memcmp(bytes.data() + 8, "WAVE", 4) == 0;
}

const TtsVoiceInfo *findId(const std::vector<TtsVoiceInfo> &voices,
                           const std::string &id) {
  for (const auto &voice : voices)
    if (voice.id == id)
      return &voice;
  return nullptr;
}

const TtsVoiceInfo *findAccent(const std::vector<TtsVoiceInfo> &voices,
                               const std::string &accent) {
  const TtsVoiceInfo *regional = nullptr;
  const std::string exact = accent == "US" ? "en-us" : "en-gb";
  for (const auto &voice : voices) {
    if (voice.accent != accent)
      continue;
    if (fold(voice.language) == exact)
      return &voice;
    if (!regional)
      regional = &voice;
  }
  return regional;
}

std::shared_ptr<TtsEngine> engine() {
  std::lock_guard<std::mutex> lock(engineMu);
  if (!slot)
    slot = createPlatformEngine();
  return slot;
}

// epochMu must be held. Active/queued requests retain their epoch across LRU eviction.
std::shared_ptr<std::atomic<std::int64_t>> clientEpoch(const std::string &clientId) {
  if (clientId.empty() || clientId.size() > 128)
    throw Error("BAD_REQUEST", "无效的朗读会话标识");
  if (!epochs.count(clientId) && epochs.size() >= 128) {
    auto victim = epochs.end();
    for (auto it = epochs.begin(); it != epochs.end(); ++it)
      if (it->second.value.use_count() == 1 &&
          (victim == epochs.end() || it->second.touched < victim->second.touched))
        victim = it;
    if (victim == epochs.end())
      throw Error("BUSY", "朗读会话过多");
    epochs.erase(victim);
  }
  auto &entry = epochs[clientId];
  entry.touched = ++touchSequence;
  return entry.value;
}

#if !LEXICA_TTS_WINRT
class UnsupportedEngine final : public TtsEngine {
public:
  TtsCatalog catalog() override {
    return {false, "当前环境不支持 Windows 系统语音合成。", {}};
  }
  TtsAudio synthesize(const TtsVoiceInfo &, const std::string &, double,
                      const std::function<bool()> &) override {
    throw Error("NOT_SUPPORTED", "当前环境不支持 Windows 系统语音合成。");
  }
  void cancel() override {}
};
#endif
} // namespace

#if !LEXICA_TTS_WINRT
std::shared_ptr<TtsEngine> createPlatformEngine() {
  return std::make_shared<UnsupportedEngine>();
}
#endif

std::string ttsInstallMessage() {
  return "没有可用的英语语音。请打开「设置 → 时间和语言 → 语言和区域」，添加「英语(美国)」或「英语(英国)」，"
         "在「安装语言功能」中勾选「文字转语音 / Text-to-speech」后安装。"
         "若该语言已经装好，点语言旁的「…」→「语言选项」，在「语言功能」里下载「文字转语音」。"
         "已安装的语音可在「设置 → 时间和语言 → 语音」中查看。";
}

void setTtsEngine(std::shared_ptr<TtsEngine> next) {
  std::lock_guard<std::mutex> lock(engineMu);
  slot = std::move(next);
}

void usePlatformTtsEngine() {
  std::lock_guard<std::mutex> lock(engineMu);
  slot = createPlatformEngine();
}

void ttsCancel(std::int64_t token, const std::string &clientId) {
  std::lock_guard<std::mutex> lock(epochMu);
  const auto epoch = clientEpoch(clientId);
  if (token > epoch->load())
    epoch->store(token);
  // Keep the active request stable until cancel() has been delivered.
  if (running && activeEpoch == epoch && runningToken < epoch->load() && activeEngine)
    activeEngine->cancel();
}

Json ttsVoices() {
  std::lock_guard<std::mutex> lock(synthMu);
  const auto catalog = engine()->catalog();
  TtsVoiceList result;
  result.supported = catalog.supported;
  if (!catalog.message.empty())
    result.message = catalog.message;
  else if (catalog.supported && catalog.voices.empty())
    result.message = ttsInstallMessage();
  for (const auto &voice : catalog.voices) {
    TtsVoice item;
    item.id = voice.id;
    item.label = voice.label;
    item.language = voice.language;
    item.accent = voice.accent;
    result.voices.push_back(std::move(item));
  }
  return result;
}

Json ttsSynthesize(const Json &args, const Cancellation *cancellation) {
  const auto input = args.get<TtsSynthesizeArgs>();
  std::shared_ptr<std::atomic<std::int64_t>> epoch;
  {
    std::lock_guard<std::mutex> lock(epochMu);
    epoch = clientEpoch(input.clientId.value_or("legacy"));
  }
  auto stale = [epoch, token = input.token, cancellation] {
    return token < epoch->load() || (cancellation && cancellation->requested());
  };
  if (stale())
    throw Error("CANCELLED", "朗读已取消");
  const auto text = trim(input.text);
  if (text.empty())
    throw Error("BAD_REQUEST", "文本为空");
  if (text.size() > kMaxText)
    throw Error("BAD_REQUEST", "文本过长");
  const double rate = input.rate.value_or(1.0);
  if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0)
    throw Error("BAD_REQUEST", "语速超出范围");
  std::optional<std::string> accent;
  if (input.accent && !input.accent->empty()) {
    if (*input.accent != "US" && *input.accent != "UK")
      throw Error("BAD_REQUEST", "口音必须是 US 或 UK");
    accent = *input.accent;
  }
  std::optional<std::string> voiceId;
  if (input.voiceId && !input.voiceId->empty()) {
    if (input.voiceId->size() > 512)
      throw Error("BAD_REQUEST", "音色标识过长");
    voiceId = *input.voiceId;
  }
  std::lock_guard<std::mutex> lock(synthMu);
  if (stale())
    throw Error("CANCELLED", "朗读已取消");
  const auto current = engine();
  struct Guard {
    ~Guard() {
      std::lock_guard<std::mutex> guard(epochMu);
      running = false;
      activeEngine.reset();
      activeEpoch.reset();
    }
  } guard;
  {
    std::lock_guard<std::mutex> mark(epochMu);
    if (stale())
      throw Error("CANCELLED", "朗读已取消");
    running = true;
    runningToken = input.token;
    activeEngine = current;
    activeEpoch = epoch;
  }
  const auto catalog = current->catalog();
  if (!catalog.supported) {
    throw Error("NOT_SUPPORTED", catalog.message.empty()
                                     ? "当前环境不支持 Windows 系统语音合成。"
                                     : catalog.message);
  }
  const TtsVoiceInfo *chosen = nullptr;
  bool substituted = false;
  if (voiceId) {
    chosen = findId(catalog.voices, *voiceId);
    if (!chosen)
      throw Error("RESOURCE_ERROR", "所选英语音色已不可用。");
    substituted = accent && chosen->accent != *accent;
  } else if (accent) {
    chosen = findAccent(catalog.voices, *accent);
    if (!chosen && !catalog.voices.empty()) {
      chosen = &catalog.voices.front();
      substituted = true;
    }
  } else if (!catalog.voices.empty()) {
    chosen = &catalog.voices.front();
  }
  if (!chosen)
    throw Error("RESOURCE_ERROR", ttsInstallMessage());
  if (stale())
    throw Error("CANCELLED", "朗读已取消");
  const auto audio = current->synthesize(*chosen, text, rate, stale);
  if (stale())
    throw Error("CANCELLED", "朗读已取消");
  if (audio.bytes.empty())
    throw Error("RESOURCE_ERROR", "合成结果为空");
  if (audio.bytes.size() > kMaxAudio)
    throw Error("RESOURCE_ERROR", "合成音频超过大小限制");
  if (!allowedMime(audio.mime))
    throw Error("RESOURCE_ERROR", "合成结果不是可播放的音频");
  if ((audio.mime == "audio/wav" || audio.mime == "audio/x-wav" ||
       audio.mime == "audio/wave") &&
      !looksLikeWav(audio.bytes))
    throw Error("RESOURCE_ERROR", "合成结果不是有效的 WAV 音频");
  TtsSynthesis result;
  result.mime = audio.mime;
  result.audio_base64 = base64Encode(audio.bytes);
  result.voice_id = chosen->id;
  result.voice_label = chosen->label;
  result.language = chosen->language;
  result.requested_accent = accent;
  result.actual_accent = chosen->accent;
  result.substituted = substituted;
  return result;
}
} // namespace lexica::parity
