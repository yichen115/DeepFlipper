#include "firmware/esp32/main/boards/deepflipper-s3/companion_json.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

extern "C" {
int companion_reference_hello(char *, size_t, unsigned);
int companion_reference_command(char *, size_t);
int companion_reference_status(char *, size_t);
int companion_reference_job(char *, size_t);
}

static cJSON *StrictParse(const char *text, int length) {
  assert(length >= 0 && size_t(length) == std::strlen(text));
  auto json = cJSON_ParseWithOpts(text, nullptr, true);
  assert(cJSON_IsObject(json));
  return json;
}

int main() {
  char text[768];
  for (uint32_t session : {272386472U, 2147483648U, 3000000000U, UINT32_MAX}) {
    int size = companion_reference_hello(text, sizeof(text), session);
    auto json = StrictParse(text, size);
    assert(dfcompanion::UInt32(json, "session") == session);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "ok")));
    assert(dfcompanion::UInt32(json, "native_rpc") == 1);
    assert(std::string(text) ==
           "{\"ok\":true,\"v\":1,\"session\":" + std::to_string(session) +
               ",\"native_rpc\":1}");
    cJSON_Delete(json);
  }
  for (auto format : {companion_reference_command, companion_reference_status,
                      companion_reference_job}) {
    int size = format(text, sizeof(text));
    auto json = StrictParse(text, size);
    cJSON_Delete(json);
    char small[8];
    assert(format(small, sizeof(small)) == size);
    assert(small[sizeof(small) - 1] == '\0');
  }
  std::cout << "PASS: real Flipper Frozen JSON to ESP32 cJSON handshake, "
               "commands, status and jobs\n";
}
