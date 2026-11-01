#include "firmware/esp32/main/boards/deepflipper-s3/preview_session.h"
#include <cassert>
#include <iostream>
using namespace dfpreview;
int main() {
  Session session;
  assert(!session.Begin("x", 0, 0) && !session.Begin("x", kMaxSamples + 1, 0));
  assert(session.Begin("job-a", 40000, 0) && session.total == 80000);
  assert(!session.Begin("job-b", 1, 0));
  assert(!session.Chunk("wrong", 0, 100, 1));
  assert(!session.Chunk("job-a", 1, 100, 1));
  assert(!session.Chunk("job-a", 0, 3, 1));
  assert(!session.Commit("job-a"));
  assert(session.Chunk("job-a", 0, kChunkBytes, 2));
  assert(!session.Chunk("job-a", 0, kChunkBytes, 3));
  assert(session.Chunk("job-a", kChunkBytes, 80000 - kChunkBytes, 4));
  assert(!session.Commit("job-b"));
  assert(session.Commit("job-a") && !session.Commit("job-a"));
  assert(!session.Chunk("job-a", 80000, 2, 5));
  assert(!session.Stop("job-b") && session.Stop("job-a") && session.Busy());
  session.stage = Stage::Cancelled;
  assert(session.Begin("job-b", kMaxSamples, 100));
  assert(!session.Expired(15100) && session.Expired(15101));
  assert(session.Chunk("job-b", 0, 2, 15000) && !session.Expired(15101));
  std::cout << "PASS: audio preview limits, ordered chunks, ownership, commit, "
               "stop and expiry\n";
}
