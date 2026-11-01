#include "firmware/esp32/main/boards/deepflipper-s3/companion_updates.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
int expansion_tx_send(void *, void *, uint8_t, uint8_t);
int expansion_tx_check(const uint8_t *, size_t, size_t);
int furi_mutex_acquire(void *mutex, uint32_t) {
  static_cast<std::mutex *>(mutex)->lock();
  return 0;
}
int furi_mutex_release(void *mutex) {
  static_cast<std::mutex *>(mutex)->unlock();
  return 0;
}
}

struct Serial {
  std::mutex bytes_mutex;
  std::vector<uint8_t> bytes;
};
extern "C" void furi_hal_serial_tx(void *handle, const uint8_t *data,
                                   size_t size) {
  auto serial = static_cast<Serial *>(handle);
  for (size_t i = 0; i < size; ++i) {
    {
      std::lock_guard<std::mutex> guard(serial->bytes_mutex);
      serial->bytes.push_back(data[i]);
    }
    // Exercise preemption within the body AND between body/checksum calls.
    std::this_thread::yield();
  }
}
extern "C" void furi_hal_serial_tx_wait_complete(void *) {
  std::this_thread::yield();
}

int main() {
  Serial serial;
  std::mutex frame_mutex;
  std::atomic<bool> start{false};
  constexpr int count = 2000;
  std::thread rpc([&] {
    while (!start.load())
      std::this_thread::yield();
    for (int i = 0; i < count; ++i)
      assert(expansion_tx_send(&frame_mutex, &serial, 5, uint8_t(i)));
  });
  std::thread worker([&] {
    while (!start.load())
      std::this_thread::yield();
    for (int i = 0; i < count; ++i) {
      assert(expansion_tx_send(&frame_mutex, &serial, 1, 0));
      assert(expansion_tx_send(&frame_mutex, &serial, 2, 0));
    }
  });
  start = true;
  rpc.join();
  worker.join();
  if (!expansion_tx_check(serial.bytes.data(), serial.bytes.size(),
                          count * 3)) {
    std::cerr << "FAIL: RPC/heartbeat/status transmit frames interleaved\n";
    return 1;
  }
  dfcompanion::Updates updates;
  int polls = 0, uis = 0, reports = 0;
  for (int64_t ms = 0; ms <= 600000; ms += 50) {
    polls += updates.Poll(ms);
    uis += updates.Ui(ms, 3, 0, 0);
    if (ms % 1000 == 0)
      reports += updates.Report(ms, "unchanged profile");
  }
  assert(polls == 3001 && uis == 301 && reports == 121);
  assert(updates.Ui(600050, 4, 0, 0));  // State change delivered immediately.
  assert(!updates.Ui(600060, 4, 1, 0)); // Coalesce sub-50 ms audio updates.
  assert(updates.Ui(600100, 4, 1, 0));
  assert(updates.Report(600050, "changed profile"));
  std::cout << "PASS: real Expansion worker concurrent TX, 6000 complete "
               "frames with forced preemption\n";
  std::cout << "PASS: companion traffic scheduling, ten minutes idle and "
               "immediate state changes\n";
}
