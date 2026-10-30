#include "parity/app/application.h"
#include "parity/modules/tts/engine.h"
#include "test_support.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
namespace p = lexica::parity;
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
p::Json call(p::Application &app, const std::string &command, p::Json args) {
  static std::atomic<std::uint64_t> sequence{0};
  return app.dispatch({{"protocol_version", 1},
                       {"request_id", "tts-" + std::to_string(++sequence)},
                       {"command", command},
                       {"args", std::move(args)}});
}
std::vector<std::uint8_t> tinyWav() {
  const std::uint32_t dataSize = 8;
  const std::uint32_t riffSize = 36 + dataSize;
  std::vector<std::uint8_t> bytes(44 + dataSize);
  auto write32 = [&](std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
  };
  auto write16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  };
  std::memcpy(bytes.data(), "RIFF", 4);
  write32(4, riffSize);
  std::memcpy(bytes.data() + 8, "WAVE", 4);
  std::memcpy(bytes.data() + 12, "fmt ", 4);
  write32(16, 16);
  write16(20, 1);
  write16(22, 1);
  write32(24, 8000);
  write32(28, 16000);
  write16(32, 2);
  write16(34, 16);
  std::memcpy(bytes.data() + 36, "data", 4);
  write32(40, dataSize);
  bytes[44] = 0x00;
  bytes[45] = 0x10;
  bytes[46] = 0xff;
  bytes[47] = 0x7f;
  bytes[48] = 0x00;
  bytes[49] = 0xf0;
  bytes[50] = 0x01;
  bytes[51] = 0x80;
  return bytes;
}
std::vector<std::uint8_t> decodeBase64(const std::string &text) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  int bits = 0;
  int count = 0;
  for (char c : text) {
    if (c == '=')
      break;
    const int n = value(c);
    if (n < 0)
      continue;
    bits = (bits << 6) | n;
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(static_cast<std::uint8_t>((bits >> count) & 0xff));
    }
  }
  return out;
}
bool pcmHasEnergy(const std::vector<std::uint8_t> &wav) {
  if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0)
    return false;
  std::size_t offset = 12;
  while (offset + 8 <= wav.size()) {
    char id[5] = {};
    std::memcpy(id, wav.data() + offset, 4);
    const auto size = static_cast<std::uint32_t>(wav[offset + 4]) |
                      (static_cast<std::uint32_t>(wav[offset + 5]) << 8) |
                      (static_cast<std::uint32_t>(wav[offset + 6]) << 16) |
                      (static_cast<std::uint32_t>(wav[offset + 7]) << 24);
    if (std::string(id) == "data") {
      const auto start = offset + 8;
      const auto end = std::min(wav.size(), start + static_cast<std::size_t>(size));
      for (std::size_t i = start; i + 1 < end; i += 2) {
        const int sample = static_cast<std::int16_t>(
            wav[i] | (static_cast<std::uint16_t>(wav[i + 1]) << 8));
        if (sample > 64 || sample < -64)
          return true;
      }
      return false;
    }
    offset += 8 + static_cast<std::size_t>(size) + (size & 1);
  }
  return false;
}
struct Scripted final : p::TtsEngine {
  p::TtsCatalog listed;
  bool fail = false;
  std::mutex mu;
  std::string text;
  double rate = 0;
  std::string voice;
  p::TtsCatalog catalog() override { return listed; }
  void cancel() override {}
  p::TtsAudio synthesize(const p::TtsVoiceInfo &voiceInfo, const std::string &value,
                         double speed, const std::function<bool()> &cancelled) override {
    if (cancelled && cancelled())
      throw p::Error("CANCELLED", "朗读已取消");
    std::lock_guard<std::mutex> lock(mu);
    text = value;
    rate = speed;
    voice = voiceInfo.id;
    if (fail)
      throw p::Error("RESOURCE_ERROR", "合成失败");
    return {"audio/wav", tinyWav()};
  }
};
struct Slow final : p::TtsEngine {
  std::atomic<int> started{0};
  std::atomic<bool> stop{false};
  p::TtsCatalog catalog() override {
    return {true, "", {{"v", "Voice", "en-US", "US"}}};
  }
  void cancel() override { stop.store(true); }
  p::TtsAudio synthesize(const p::TtsVoiceInfo &, const std::string &, double,
                         const std::function<bool()> &cancelled) override {
    started.fetch_add(1);
    stop.store(false);
    for (int i = 0; i < 150; ++i) {
      if (stop.load() || (cancelled && cancelled()))
        throw p::Error("CANCELLED", "朗读已取消");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {"audio/wav", tinyWav()};
  }
};
p::Json synth(p::Application &app, const std::string &text, std::int64_t token,
              p::Json extra = p::Json::object()) {
  extra["text"] = text;
  extra["token"] = token;
  return call(app, "tts_synthesize", std::move(extra));
}
int main() {
  try {
    const auto dir = test_support::freshDirectory("parity", "tts");
    p::Application app(dir / "lexica.db");
    auto scripted = std::make_shared<Scripted>();
    scripted->listed = {true, "", {{"uk", "Hazel", "en-GB", "UK"}, {"us", "Zira", "en-US", "US"}}};
    p::setTtsEngine(scripted);
    const auto voices = call(app, "tts_voices", p::Json::object());
    check(voices["ok"] == true && voices["result"]["voices"].size() == 2, voices.dump());
    check(call(app, "tts_cancel", {{"token", 1}})["ok"] == true, "cancel");
    check(synth(app, "  ", 2)["error"]["code"] == "BAD_REQUEST", "blank text");
    check(synth(app, std::string(2001, 'a'), 3)["error"]["code"] == "BAD_REQUEST", "long text");
    check(synth(app, "hello", 4, {{"rate", 4}})["error"]["code"] == "BAD_REQUEST", "rate");
    check(synth(app, "hello", 5, {{"accent", "AU"}})["error"]["code"] == "BAD_REQUEST", "accent");
    check(synth(app, "hello", 6, {{"voiceId", "missing"}, {"accent", "UK"}})["error"]["code"] ==
              "RESOURCE_ERROR",
          "missing voice");
    const auto uk = synth(app, " meticulous ", 7, {{"accent", "UK"}, {"rate", 0.8}});
    check(uk["ok"] == true && uk["result"]["voice_id"] == "uk" &&
              uk["result"]["substituted"] == false && uk["result"]["actual_accent"] == "UK" &&
              uk["result"]["requested_accent"] == "UK",
          uk.dump());
    check(scripted->text == "meticulous" && scripted->rate == 0.8 && scripted->voice == "uk",
          "rate and voice were not applied");
    scripted->listed.voices = {{"us", "Zira", "en-US", "US"}};
    const auto substituted = synth(app, "The early bird catches the worm.", 9, {{"accent", "UK"}, {"rate", 1.2}});
    check(substituted["ok"] == true && substituted["result"]["substituted"] == true &&
              substituted["result"]["actual_accent"] == "US" &&
              substituted["result"]["voice_label"] == "Zira",
          substituted.dump());
    check(scripted->rate == 1.2 && scripted->text == "The early bird catches the worm.",
          "sentence or rate lost");
    scripted->listed.voices.clear();
    scripted->listed.message.clear();
    const auto none = synth(app, "hello", 10, {{"accent", "US"}});
    check(none["ok"] == false && none["error"]["code"] == "RESOURCE_ERROR" &&
              std::string(none["error"]["message"]).find("文字转语音") != std::string::npos,
          none.dump());
    const auto listed = call(app, "tts_voices", p::Json::object());
    check(listed["result"]["supported"] == true && listed["result"]["voices"].empty() &&
              std::string(listed["result"]["message"]).find("文字转语音") != std::string::npos,
          listed.dump());
    scripted->listed.voices = {{"us", "Zira", "en-US", "US"}};
    scripted->fail = true;
    const auto failed = synth(app, "hello", 11, {{"voiceId", "us"}, {"rate", 1}});
    check(failed["error"]["code"] == "RESOURCE_ERROR" && failed["error"]["message"] == "合成失败",
          failed.dump());
    scripted->fail = false;
    check(call(app, "tts_cancel", {{"token", 20}})["ok"] == true, "raise epoch");
    check(synth(app, "hello", 19, {{"voiceId", "us"}})["error"]["code"] == "CANCELLED",
          "stale token returned audio");
    check(call(app, "tts_cancel", {{"token", 1000}, {"clientId", "old-document"}})["ok"] == true,
          "old document cancellation");
    check(synth(app, "hello", 1, {{"clientId", "new-document"}})["ok"] == true,
          "a reload or another tab inherited the old cancellation epoch");
    check(synth(app, "hello", 1, {{"clientId", "old-document"}})["error"]["code"] == "CANCELLED",
          "client-scoped stale token was accepted");
    auto slow = std::make_shared<Slow>();
    p::setTtsEngine(slow);
    p::Json first;
    std::thread worker([&] { first = synth(app, "hello", 21, {{"voiceId", "v"}}); });
    for (int i = 0; i < 100 && slow->started.load() == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(slow->started.load() == 1, "slow synthesis did not start");
    call(app, "tts_cancel", {{"token", 2000}, {"clientId", "other-tab"}});
    check(!slow->stop.load(), "another tab cancelled active synthesis");
    check(call(app, "tts_cancel", {{"token", 22}})["ok"] == true, "cancel in flight");
    worker.join();
    check(first["ok"] == false && first["error"]["code"] == "CANCELLED" && !first.contains("result"),
          first.dump());
    p::usePlatformTtsEngine();
    const auto liveVoices = call(app, "tts_voices", p::Json::object());
    check(liveVoices["ok"] == true, liveVoices.dump());
    std::ofstream report(dir / "tts-report.txt");
    report << liveVoices["result"].dump(2) << "\n";
    if (liveVoices["result"]["supported"] == false) {
      const auto denied = synth(app, "hello", 30, {{"accent", "US"}, {"rate", 1}});
      check(denied["error"]["code"] == "NOT_SUPPORTED", denied.dump());
      report << "platform=unsupported\n";
      std::cout << "tts platform unsupported\n";
      return 0;
    }
    if (liveVoices["result"]["voices"].empty()) {
      const auto denied = synth(app, "hello", 30, {{"accent", "US"}, {"rate", 1}});
      check(denied["error"]["code"] == "RESOURCE_ERROR", denied.dump());
      check(std::string(denied["error"]["message"]).find("文字转语音") != std::string::npos,
            denied.dump());
      report << "platform=no-english-voice\n";
      std::cout << "tts has no English voice\n";
      return 0;
    }
    const auto word = synth(app, "hello", 30, {{"accent", "US"}, {"rate", 1}});
    check(word["ok"] == true, word.dump());
    const auto wordBytes = decodeBase64(word["result"]["audio_base64"]);
    check(pcmHasEnergy(wordBytes), "word audio is missing or silent");
    std::ofstream(dir / "hello.wav", std::ios::binary)
        .write(reinterpret_cast<const char *>(wordBytes.data()),
               static_cast<std::streamsize>(wordBytes.size()));
    const auto sentence = synth(app, "The early bird catches the worm.", 31,
                                {{"accent", "UK"}, {"rate", 0.8}});
    check(sentence["ok"] == true, sentence.dump());
    const auto sentenceBytes = decodeBase64(sentence["result"]["audio_base64"]);
    check(pcmHasEnergy(sentenceBytes), "sentence audio is missing or silent");
    std::ofstream(dir / "sentence.wav", std::ios::binary)
        .write(reinterpret_cast<const char *>(sentenceBytes.data()),
               static_cast<std::streamsize>(sentenceBytes.size()));
    report << "word_voice=" << word["result"]["voice_label"] << " "
           << word["result"]["language"] << " substituted=" << word["result"]["substituted"]
           << "\n";
    report << "sentence_voice=" << sentence["result"]["voice_label"] << " "
           << sentence["result"]["language"]
           << " substituted=" << sentence["result"]["substituted"] << "\n";
    std::cout << "tts synthesized " << wordBytes.size() << " and " << sentenceBytes.size()
              << " bytes\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
