#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <vector>
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
enum AudioTaskType { kAudioTaskTypeDecodeToPlaybackQueue };
struct AudioTask {
  AudioTaskType type;
  std::vector<int16_t> pcm;
  uint32_t timestamp, playback_id, media_position_ms;
};
struct Codec {
  int output_sample_rate() { return 24000; }
};
class AudioService {
public:
  Codec codec;
  Codec *codec_ = &codec;
  std::mutex audio_queue_mutex_;
  std::condition_variable audio_queue_cv_;
  std::atomic<bool> service_stopped_{false};
  std::deque<AudioTask> audio_playback_queue_;
  bool playback_drained_notified_ = true;
  bool PushPcmToPlaybackQueue(std::vector<int16_t> &pcm);
};
#include "pcm-queue-production.inc"
int main() {
  AudioService audio;
  std::vector<int16_t> empty, large(1441, 1), first(1440, 123),
      second(100, -321), third(100, 8);
  assert(!audio.PushPcmToPlaybackQueue(empty) &&
         !audio.PushPcmToPlaybackQueue(large));
  assert(audio.PushPcmToPlaybackQueue(first) && first.empty() &&
         !audio.playback_drained_notified_);
  assert(audio.PushPcmToPlaybackQueue(second) && second.empty());
  assert(!audio.PushPcmToPlaybackQueue(third) && third.size() == 100);
  assert(audio.audio_playback_queue_[0].pcm[0] == 123 &&
         audio.audio_playback_queue_[1].pcm[0] == -321);
  audio.audio_playback_queue_.pop_front();
  audio.service_stopped_ = true;
  assert(!audio.PushPcmToPlaybackQueue(third) && third.size() == 100);
  std::cout << "PASS: actual PCM playback queue bounds, move ownership and "
               "stopped service\n";
}
