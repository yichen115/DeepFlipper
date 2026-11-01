#include <cassert>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>
enum State {
  kDeviceStateStarting,
  kDeviceStateActivating,
  kDeviceStateWifiConfiguring,
  kDeviceStateIdle,
  kDeviceStateNotifying,
  kDeviceStateListening
};
struct Board {
  bool direct = true;
  static Board &GetInstance() {
    static Board b;
    return b;
  }
  bool UseDirectVoiceServer() { return direct; }
};
struct Audio {
  int resets = 0;
  void EnableVoiceProcessing(bool) {}
  void EnableWakeWordDetection(bool) {}
  void ResetDecoder() { ++resets; }
};
struct Protocol {
  int closed = 0;
  void CloseAudioChannel() { ++closed; }
};
class Application {
public:
  State state = kDeviceStateIdle;
  Audio audio_service_;
  Protocol protocol;
  Protocol *protocol_ = &protocol;
  std::vector<std::function<void()>> tasks;
  void Schedule(std::function<void()> fn) { tasks.push_back(std::move(fn)); }
  State GetDeviceState() { return state; }
  void SetDeviceState(State s) { state = s; }
  void StopNotification() {}
  void SuspendVoiceSession(std::function<void()> done = {},
                           std::function<bool()> guard = {});
  void Drain() {
    auto pending = std::move(tasks);
    for (auto &task : pending)
      task();
  }
};
#include "preview-suspend-production.inc"
int main() {
  Application app;
  bool owns = true;
  int finished = 0;
  app.SuspendVoiceSession([&] { ++finished; }, [&] { return owns; });
  owns = false;
  app.state = kDeviceStateListening;
  app.Drain();
  assert(finished == 1 && app.audio_service_.resets == 0 &&
         app.protocol.closed == 0 && app.state == kDeviceStateListening);
  owns = true;
  app.state = kDeviceStateIdle;
  app.SuspendVoiceSession([&] { ++finished; }, [&] { return owns; });
  app.Drain();
  assert(finished == 2 && app.audio_service_.resets == 1 &&
         app.protocol.closed == 1);
  app.state = kDeviceStateStarting;
  app.SuspendVoiceSession([&] { ++finished; });
  app.Drain();
  assert(finished == 3 && app.state == kDeviceStateStarting &&
         app.protocol.closed == 1);
  std::cout << "PASS: actual preview preparation completion and cancelled "
               "lease cannot suspend a new recording\n";
}
