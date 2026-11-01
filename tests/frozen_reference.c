/* Compile the actual firmware formatter using the host's C99 libc. */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t host_strlcat(char *dest, const char *src, size_t capacity) {
  size_t used = 0, length = strlen(src);
  while (used < capacity && dest[used])
    ++used;
  if (used < capacity) {
    size_t copied = length < capacity - used - 1 ? length : capacity - used - 1;
    memcpy(dest + used, src, copied);
    dest[used + copied] = '\0';
  }
  return used + length;
}
#define strlcat host_strlcat
#ifdef _WIN32
#undef _WIN32
#endif
#include "firmware/flipper/lib/mjs/common/frozen/frozen.c"

int companion_reference_hello(char *output, size_t capacity, unsigned session) {
  struct json_out out = JSON_OUT_BUF(output, capacity);
  return json_printf(&out, "{ok:%B,v:1,session:%u,native_rpc:1}", true,
                     session);
}

int companion_reference_command(char *output, size_t capacity) {
  struct json_out out = JSON_OUT_BUF(output, capacity);
  return json_printf(&out, "{id:%u,field:%Q,value:%Q}", 7U, "volume", "20");
}

int companion_reference_status(char *output, size_t capacity) {
  struct json_out out = JSON_OUT_BUF(output, capacity);
  return json_printf(
      &out, "{ok:%B,state:%d,foreground:%B,locked:%B,volume:%d,tools:[%Q,%Q]}",
      true, 2, false, false, 20, "flipper_get_status", "flipper_nfc_scan");
}

int companion_reference_job(char *output, size_t capacity) {
  struct json_out out = JSON_OUT_BUF(output, capacity);
  return json_printf(&out, "{ok:%B,job:%u,state:%Q,protocol:%Q}", true, 42U,
                     "done", "ISO14443-3A");
}
