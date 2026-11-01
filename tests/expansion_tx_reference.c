/* The build extracts the two unmodified transmit functions from the worker.
 * Only the RTOS mutex and UART driver are replaced by host implementations. */
#include "firmware/flipper/applications/services/expansion/expansion_protocol.h"
#include <string.h>

typedef void FuriMutex;
typedef int FuriStatus;
#define FuriStatusOk 0
#define FuriWaitForever UINT32_MAX
FuriStatus furi_mutex_acquire(FuriMutex *, uint32_t);
FuriStatus furi_mutex_release(FuriMutex *);
void furi_hal_serial_tx(void *, const uint8_t *, size_t);
void furi_hal_serial_tx_wait_complete(void *);
typedef struct {
  FuriMutex *tx_mutex;
  void *serial_handle;
} ExpansionWorker;

#include "expansion-tx-production.inc"

int expansion_tx_send(void *mutex, void *serial, uint8_t type, uint8_t value) {
  ExpansionWorker worker = {.tx_mutex = mutex, .serial_handle = serial};
  ExpansionFrame frame = {.header.type = type};
  if (type == ExpansionFrameTypeData) {
    frame.content.data.size = 64;
    memset(frame.content.data.bytes, value, 64);
  } else if (type == ExpansionFrameTypeStatus) {
    frame.content.status.error = ExpansionFrameErrorNone;
  }
  return expansion_worker_send_frame(&worker, &frame);
}

typedef struct {
  const uint8_t *data;
  size_t size, pos;
} Capture;
static size_t read_capture(uint8_t *dest, size_t size, void *context) {
  Capture *capture = context;
  if (size > capture->size - capture->pos)
    return 0;
  memcpy(dest, capture->data + capture->pos, size);
  capture->pos += size;
  return size;
}
int expansion_tx_check(const uint8_t *data, size_t size, size_t expected) {
  Capture capture = {.data = data, .size = size};
  size_t frames = 0;
  while (capture.pos < size) {
    ExpansionFrame frame;
    if (expansion_protocol_decode(&frame, read_capture, &capture) !=
        ExpansionProtocolStatusOk)
      return 0;
    if (frame.header.type == ExpansionFrameTypeData) {
      if (frame.content.data.size != 64)
        return 0;
      for (size_t i = 1; i < 64; ++i)
        if (frame.content.data.bytes[i] != frame.content.data.bytes[0])
          return 0;
    }
    ++frames;
  }
  return frames == expected;
}
