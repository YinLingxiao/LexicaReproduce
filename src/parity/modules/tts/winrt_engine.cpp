#include "parity/modules/tts/engine.h"
#include <sapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <atomic>
#include <chrono>
#include <type_traits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
namespace lexica::parity {
namespace {
constexpr std::size_t kMaxAudio = 768 * 1024;
namespace media = winrt::Windows::Media::SpeechSynthesis;
namespace streams = winrt::Windows::Storage::Streams;

std::string clip(std::string value) {
  if (value.size() > 240)
    value.resize(240);
  return value;
}

std::wstring widen(const std::string &text) {
  if (text.empty())
    return {};
  const auto chars = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(chars > 0 ? static_cast<std::size_t>(chars) : 0, L'\0');
  if (chars > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), chars);
  return out;
}

std::string narrow(const wchar_t *text) {
  if (!text || !*text)
    return {};
  const auto bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(bytes > 1 ? static_cast<std::size_t>(bytes - 1) : 0, '\0');
  if (bytes > 1)
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

struct ComString {
  wchar_t *value = nullptr;
  ~ComString() { CoTaskMemFree(value); }
  std::string str() const { return narrow(value); }
};

std::string sapiAccent(unsigned language) {
  if (PRIMARYLANGID(language) != LANG_ENGLISH)
    return {};
  if (SUBLANGID(language) == SUBLANG_ENGLISH_US)
    return "US";
  if (SUBLANGID(language) == SUBLANG_ENGLISH_UK)
    return "UK";
  return "OTHER";
}

std::string sapiLanguage(unsigned language) {
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (LCIDToLocaleName(language, name, LOCALE_NAME_MAX_LENGTH, 0))
    return narrow(name);
  return "en";
}

void appendSapiVoices(TtsCatalog &result) {
  ISpObjectTokenCategory *category = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                              IID_ISpObjectTokenCategory, reinterpret_cast<void **>(&category))))
    return;
  if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) {
    category->Release();
    return;
  }
  IEnumSpObjectTokens *tokens = nullptr;
  const auto listed = category->EnumTokens(nullptr, nullptr, &tokens);
  category->Release();
  if (FAILED(listed) || !tokens)
    return;
  for (;;) {
    ISpObjectToken *token = nullptr;
    ULONG fetched = 0;
    if (tokens->Next(1, &token, &fetched) != S_OK || !token)
      break;
    ComString id;
    ComString language;
    token->GetId(&id.value);
    ISpDataKey *attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
      attributes->GetStringValue(L"Language", &language.value);
      ComString name;
      attributes->GetStringValue(L"Name", &name.value);
      attributes->Release();
      const auto lang = language.value ? std::wcstoul(language.value, nullptr, 16) : 0u;
      const auto accent = sapiAccent(static_cast<unsigned>(lang));
      const auto voiceId = id.str();
      if (!accent.empty() && !voiceId.empty()) {
        bool duplicate = false;
        for (const auto &have : result.voices)
          if (have.id == voiceId)
            duplicate = true;
        if (!duplicate) {
          auto label = name.str();
          if (label.empty())
            label = voiceId;
          result.voices.push_back(
              TtsVoiceInfo{voiceId, std::move(label), sapiLanguage(static_cast<unsigned>(lang)), accent});
        }
      }
    }
    token->Release();
  }
  tokens->Release();
}

TtsAudio synthesizeSapi(const TtsVoiceInfo &voice, const std::string &text, double rate,
                        const std::function<bool()> &cancelled) {
  if (cancelled())
    throw Error("CANCELLED", "朗读已取消");
  IEnumSpObjectTokens *tokens = nullptr;
  ISpObjectTokenCategory *category = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                              IID_ISpObjectTokenCategory, reinterpret_cast<void **>(&category))))
    throw Error("RESOURCE_ERROR", "所选英语音色已不可用。");
  if (FAILED(category->SetId(SPCAT_VOICES, FALSE)) ||
      FAILED(category->EnumTokens(nullptr, nullptr, &tokens))) {
    category->Release();
    throw Error("RESOURCE_ERROR", "所选英语音色已不可用。");
  }
  category->Release();
  ISpObjectToken *selected = nullptr;
  for (;;) {
    ISpObjectToken *token = nullptr;
    ULONG fetched = 0;
    if (tokens->Next(1, &token, &fetched) != S_OK || !token)
      break;
    ComString id;
    token->GetId(&id.value);
    if (id.str() == voice.id && !selected)
      selected = token;
    else
      token->Release();
  }
  tokens->Release();
  if (!selected)
    throw Error("RESOURCE_ERROR", "所选英语音色已不可用。");
  ISpVoice *speaker = nullptr;
  IStream *base = nullptr;
  ISpStream *stream = nullptr;
  auto release = [&] {
    if (speaker)
      speaker->Release();
    if (stream)
      stream->Release();
    if (base)
      base->Release();
    selected->Release();
  };
  if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              reinterpret_cast<void **>(&speaker))) ||
      FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &base)) ||
      FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                              reinterpret_cast<void **>(&stream)))) {
    release();
    throw Error("RESOURCE_ERROR", "语音合成失败。");
  }
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 1;
  format.nSamplesPerSec = 22050;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 2;
  format.nAvgBytesPerSec = 44100;
  if (FAILED(stream->SetBaseStream(base, SPDFID_WaveFormatEx, &format)) ||
      FAILED(speaker->SetVoice(selected)) || FAILED(speaker->SetOutput(stream, TRUE))) {
    release();
    throw Error("RESOURCE_ERROR", "语音合成失败。");
  }
  const long sapiRate = rate <= 0.9 ? -2 : rate >= 1.1 ? 2 : 0;
  speaker->SetRate(sapiRate);
  const auto spoken = speaker->Speak(widen(text).c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
  if (FAILED(spoken)) {
    release();
    throw Error("RESOURCE_ERROR", "语音合成失败。");
  }
  for (;;) {
    if (cancelled()) {
      speaker->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
      release();
      throw Error("CANCELLED", "朗读已取消");
    }
    if (speaker->WaitUntilDone(50) == S_OK)
      break;
  }
  if (cancelled()) {
    release();
    throw Error("CANCELLED", "朗读已取消");
  }
  stream->Close();
  stream->Release();
  stream = nullptr;
  LARGE_INTEGER zero{};
  base->Seek(zero, STREAM_SEEK_SET, nullptr);
  STATSTG stat{};
  if (FAILED(base->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart <= 0 ||
      static_cast<std::uint64_t>(stat.cbSize.QuadPart) > kMaxAudio) {
    release();
    throw Error("RESOURCE_ERROR", stat.cbSize.QuadPart <= 0 ? "合成结果为空" : "合成音频超过大小限制");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
  ULONG read = 0;
  const auto copied = base->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
  release();
  if (FAILED(copied) || read == 0)
    throw Error("RESOURCE_ERROR", "合成结果为空");
  bytes.resize(read);
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    const auto pcm = bytes.size();
    std::vector<std::uint8_t> wav(44 + pcm);
    auto write32 = [&](std::size_t offset, std::uint32_t value) {
      wav[offset] = static_cast<std::uint8_t>(value);
      wav[offset + 1] = static_cast<std::uint8_t>(value >> 8);
      wav[offset + 2] = static_cast<std::uint8_t>(value >> 16);
      wav[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    auto write16 = [&](std::size_t offset, std::uint16_t value) {
      wav[offset] = static_cast<std::uint8_t>(value);
      wav[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    std::memcpy(wav.data(), "RIFF", 4);
    write32(4, static_cast<std::uint32_t>(36 + pcm));
    std::memcpy(wav.data() + 8, "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    write32(16, 16);
    write16(20, 1);
    write16(22, 1);
    write32(24, 22050);
    write32(28, 44100);
    write16(32, 2);
    write16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    write32(40, static_cast<std::uint32_t>(pcm));
    std::memcpy(wav.data() + 44, bytes.data(), pcm);
    bytes = std::move(wav);
  }
  return {"audio/wav", std::move(bytes)};
}

class WinRtEngine final : public TtsEngine {
public:
  WinRtEngine() {
    worker = std::thread([this] { loop(); });
  }
  ~WinRtEngine() override {
    cancel();
    {
      std::lock_guard<std::mutex> lock(queueMu);
      stop = true;
    }
    cv.notify_one();
    if (worker.joinable())
      worker.join();
  }
  TtsCatalog catalog() override {
    return wait(post([this] { return catalogOnWorker(); }));
  }
  TtsAudio synthesize(const TtsVoiceInfo &voice, const std::string &text, double rate,
                      const std::function<bool()> &cancelled) override {
    return wait(post([this, voice, text, rate, cancelled] {
      return synthesizeOnWorker(voice, text, rate, cancelled);
    }));
  }
  void cancel() override {
    cancelFlag.store(true);
    try {
      std::lock_guard<std::mutex> lock(opMu);
      if (operation)
        operation.Cancel();
    } catch (...) {
    }
  }

private:
  template <typename F> auto post(F &&job) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    using Task = std::packaged_task<std::invoke_result_t<std::decay_t<F>>()>;
    auto task = std::make_shared<Task>(std::forward<F>(job));
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(queueMu);
      if (stop)
        throw Error("CANCELLED", "朗读已取消");
      if (jobs.size() >= 2)
        throw Error("BUSY", "语音任务过多");
      jobs.push_back([task] { (*task)(); });
    }
    cv.notify_one();
    return future;
  }
  template <typename T> T wait(std::future<T> future) {
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
      cancel();
      if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
        throw Error("RESOURCE_ERROR", "语音合成超时");
    }
    return future.get();
  }
  void loop() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      ready = true;
    } catch (const winrt::hresult_error &error) {
      initMessage = "当前环境不支持 Windows 系统语音合成。（" +
                    clip(winrt::to_string(error.message())) + "）";
    } catch (...) {
      initMessage = "当前环境不支持 Windows 系统语音合成。";
    }
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(queueMu);
        cv.wait(lock, [&] { return stop || !jobs.empty(); });
        if (jobs.empty())
          break;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
    if (ready)
      winrt::uninit_apartment();
  }
  TtsCatalog catalogOnWorker() {
    TtsCatalog result;
    result.supported = ready;
    if (!ready)
      result.message = initMessage.empty() ? "当前环境不支持 Windows 系统语音合成。" : initMessage;
    try {
      if (!ready)
        throw winrt::hresult_error(E_FAIL);
      const auto all = media::SpeechSynthesizer::AllVoices();
      for (std::uint32_t index = 0; index < all.Size(); ++index) {
        const auto voice = all.GetAt(index);
        const auto language = winrt::to_string(voice.Language());
        const auto accent = ttsLanguageAccent(language);
        if (accent.empty())
          continue;
        auto label = winrt::to_string(voice.DisplayName());
        if (label.empty())
          label = winrt::to_string(voice.Description());
        if (label.empty())
          label = language;
        auto id = winrt::to_string(voice.Id());
        if (id.empty())
          continue;
        bool duplicate = false;
        for (const auto &have : result.voices)
          if (have.id == id)
            duplicate = true;
        if (!duplicate)
          result.voices.push_back(
              TtsVoiceInfo{std::move(id), std::move(label), language, accent});
      }
    } catch (const winrt::hresult_error &error) {
      if (ready) {
        result.voices.clear();
        result.message = ttsInstallMessage() + "（" +
                         clip(winrt::to_string(error.message())) + "）";
      }
    }
    try {
      appendSapiVoices(result);
    } catch (...) {
    }
    if (!result.voices.empty()) {
      result.supported = true;
      result.message.clear();
    }
    if (result.voices.empty() && result.message.empty())
      result.message = ttsInstallMessage();
    return result;
  }
  TtsAudio synthesizeOnWorker(const TtsVoiceInfo &voice, const std::string &text,
                              double rate, const std::function<bool()> &cancelled) {
    if (!ready) {
      try {
        return synthesizeSapi(voice, text, rate, cancelled);
      } catch (const Error &error) {
        if (error.code == "RESOURCE_ERROR" && error.what() == std::string("所选英语音色已不可用。"))
          throw Error("NOT_SUPPORTED", initMessage.empty()
                                           ? "当前环境不支持 Windows 系统语音合成。"
                                           : initMessage);
        throw;
      }
    }
    if (cancelled())
      throw Error("CANCELLED", "朗读已取消");
    cancelFlag.store(false);
    if (cancelled())
      throw Error("CANCELLED", "朗读已取消");
    media::SpeechSynthesizer speaker;
    bool found = false;
    const auto all = media::SpeechSynthesizer::AllVoices();
    for (std::uint32_t index = 0; index < all.Size(); ++index) {
      const auto candidate = all.GetAt(index);
      if (winrt::to_string(candidate.Id()) == voice.id) {
        speaker.Voice(candidate);
        found = true;
        break;
      }
    }
    if (!found)
      return synthesizeSapi(voice, text, rate, cancelled);
    speaker.Options().SpeakingRate(rate);
    auto started = speaker.SynthesizeTextToStreamAsync(winrt::to_hstring(text));
    {
      std::lock_guard<std::mutex> lock(opMu);
      operation = started;
      if (cancelFlag.load() || cancelled())
        operation.Cancel();
    }
    media::SpeechSynthesisStream stream{nullptr};
    try {
      stream = started.get();
    } catch (const winrt::hresult_canceled &) {
      throw Error("CANCELLED", "朗读已取消");
    } catch (const winrt::hresult_error &error) {
      throw Error("RESOURCE_ERROR",
                  "语音合成失败：" + clip(winrt::to_string(error.message())));
    }
    {
      std::lock_guard<std::mutex> lock(opMu);
      operation = nullptr;
    }
    if (cancelled())
      throw Error("CANCELLED", "朗读已取消");
    const auto size = stream.Size();
    if (size == 0)
      throw Error("RESOURCE_ERROR", "合成结果为空");
    if (size > kMaxAudio)
      throw Error("RESOURCE_ERROR", "合成音频超过大小限制");
    streams::DataReader reader{stream.GetInputStreamAt(0)};
    const auto loaded = reader.LoadAsync(static_cast<std::uint32_t>(size)).get();
    if (loaded == 0)
      throw Error("RESOURCE_ERROR", "合成结果为空");
    std::vector<std::uint8_t> bytes(loaded);
    reader.ReadBytes(winrt::array_view<std::uint8_t>(
        bytes.data(), bytes.data() + bytes.size()));
    reader.Close();
    auto mime = winrt::to_string(stream.ContentType());
    stream.Close();
    for (char &c : mime)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto semi = mime.find(';');
    if (semi != std::string::npos)
      mime.resize(semi);
    while (!mime.empty() && (mime.back() == ' ' || mime.back() == '\t'))
      mime.pop_back();
    if ((mime.empty() || mime == "application/octet-stream") &&
        bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0)
      mime = "audio/wav";
    return {std::move(mime), std::move(bytes)};
  }
  std::mutex queueMu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  std::thread worker;
  bool stop = false;
  bool ready = false;
  std::string initMessage;
  std::mutex opMu;
  winrt::Windows::Foundation::IAsyncOperation<media::SpeechSynthesisStream> operation{
      nullptr};
  std::atomic<bool> cancelFlag{false};
};
} // namespace
std::shared_ptr<TtsEngine> createPlatformEngine() {
  return std::make_shared<WinRtEngine>();
}
} // namespace lexica::parity
