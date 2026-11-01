#include "firmware/esp32/main/boards/deepflipper-s3/speaker_pcm.h"
#include <cassert>
#include <climits>
#include <iostream>

int main() {
  const int16_t pcm[] = {INT16_MIN, -1, 0, 1, INT16_MAX};
  int32_t slots[10];
  dfspeaker::PackStereo(pcm, 5, 100, slots);
  assert(slots[0] == INT32_MIN && slots[8] == 2147418112);
  for (size_t i = 0; i < 5; ++i) {
    assert(slots[2 * i] == slots[2 * i + 1]);
    assert(slots[2 * i] == int64_t(pcm[i]) * 65536);
  }
  dfspeaker::PackStereo(pcm, 5, 0, slots);
  for (auto slot : slots)
    assert(slot == 0);
  dfspeaker::PackStereo(pcm, 5, 50, slots);
  assert(slots[0] == INT32_MIN / 4 && slots[8] == 2147418112 / 4);
  std::cout << "PASS: board I2S stereo slots, signed PCM alignment, volume and "
               "mute\n";
}
