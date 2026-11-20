/* Compile the complete desktop renderer with deterministic SD/RTC/GUI I/O. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  bool held;
} FuriMutex;
typedef struct {
  int hour, minute;
} DateTime;
typedef struct {
  int unused;
} Storage;
typedef struct {
  int which;
  size_t at;
} File;
typedef struct {
  uint8_t pixels[1024];
} Canvas;
enum {
  FuriMutexTypeNormal,
  FuriWaitForever,
  FSAM_READ,
  FSOM_OPEN_EXISTING,
  FuriHalRtcFlagLock,
  LocaleTimeFormat12h,
  LocaleTimeFormat24h,
};
#define EXT_PATH(path) "/ext/" path
#define RECORD_STORAGE "storage"
#include "firmware/flipper/applications/services/deepflipper/deepflipper.h"
static DeepFlipperSnapshot snapshot;
static DateTime date = {12, 34};
static uint32_t ticks = 120, offset;
static unsigned sd_calls, hint_reads;
static bool missing, bad_size, seek_error, short_read, locked, drawing;
static bool hint_present;

static uint8_t hint[44];
static int time_format = LocaleTimeFormat24h;
static FuriMutex mutex;
static Storage storage;
static File file;
static uint8_t *atlas;
static size_t atlas_size;
static FuriMutex *furi_mutex_alloc(int type) {
  (void)type;
  return &mutex;
}
static void furi_mutex_acquire(FuriMutex *m, int wait) {
  (void)wait;
  assert(!m->held);
  m->held = true;
}
static void furi_mutex_release(FuriMutex *m) {
  assert(m->held);
  m->held = false;
}
static uint32_t furi_get_tick(void) { return ticks; }
static uint32_t furi_ms_to_ticks(uint32_t ms) { return ms; }
static void furi_hal_rtc_get_datetime(DateTime *out) { *out = date; }
static uint32_t datetime_datetime_to_timestamp(DateTime *d) {
  return 1000000 + d->hour * 3600 + d->minute * 60;
}
static bool furi_hal_rtc_is_flag_set(int flag) {
  (void)flag;
  return locked;
}
static int locale_get_time_format(void) { return time_format; }
void deepflipper_snapshot(DeepFlipperSnapshot *out) { *out = snapshot; }
static Storage *furi_record_open(const char *record) {
  assert(!drawing && !mutex.held && strcmp(record, RECORD_STORAGE) == 0);
  ++sd_calls;
  return &storage;
}
static void furi_record_close(const char *record) { (void)record; }
static File *storage_file_alloc(Storage *value) {
  assert(value == &storage);
  return &file;
}
static bool storage_file_open(File *f, const char *path, int access, int mode) {
  assert(f == &file && access == FSAM_READ && mode == FSOM_OPEN_EXISTING);
  f->at = 0;
  if (strcmp(path, "/ext/deepflipper/pet/home.bin") == 0) {
    f->which = 1;
    ++hint_reads;
    return hint_present;
  }
  assert(strcmp(path, "/ext/deepflipper/home_frames.bin") == 0);
  f->which = 0;
  return !missing;
}
static size_t storage_file_size(File *f) {
  assert(f->which == 0);
  return atlas_size - bad_size;
}
static bool storage_file_seek(File *f, uint32_t position, bool absolute) {
  assert(f->which == 0 && absolute && position + 1024 <= atlas_size);
  offset = position;
  return !seek_error;
}
static size_t storage_file_read(File *f, void *data, size_t size) {
  if (f->which == 1) {
    assert(size == 44);
    memcpy(data, hint, 44);
    return 44;
  }
  assert(size == 1024);
  size -= short_read;
  memcpy(data, atlas + offset, size);
  return size;
}
static void storage_file_close(File *f) { (void)f; }
static void storage_file_free(File *f) { (void)f; }
static void canvas_clear(Canvas *c) { memset(c, 0, sizeof(*c)); }
static void canvas_draw_xbm(Canvas *c, int x, int y, size_t w, size_t h,
                            const uint8_t *data) {
  assert(mutex.held && x == 0 && y == 0 && w == 128 && h == 64);
  memcpy(c->pixels, data, 1024);
}
#include "firmware/flipper/applications/services/deepflipper/deepflipper_sprite.c"
#include "firmware/flipper/applications/examples/whale_pet/pet_model.h"

static bool ink(const uint8_t *frame, int x, int y) {
  return frame[y * 16 + x / 8] >> (x % 8) & 1;
}
/* Regions the renderer is allowed to change on top of the baked room. */
static bool overlay(int x, int y) {
  return (x >= 4 && x <= 28 && y >= 24 && y <= 34) ||  /* clock screen */
         (x >= 101 && x <= 124 && y >= 28 && y <= 37) || /* cooker screen */
         (x >= 85 && x <= 102 && y <= 13) ||             /* bubble */
         (x >= 106 && x <= 117 && y >= 8 && y <= 15) ||  /* steam */
         (x >= 106 && y >= 55) ||                        /* lock over mat */
         (x >= 58 && x <= 62 && y >= 33 && y <= 36);     /* live mouth */
}
static void update(void) {
  ticks += 120;
  deepflipper_sprite_update(&snapshot);
}
static const uint8_t *base(void) { return atlas + offset; }
static void outside_overlays_match(void) {
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 128; x++)
      if (!overlay(x, y))
        assert(ink(bitmap, x, y) == ink(base(), x, y));
}
static bool any_ink(int x0, int y0, int x1, int y1) {
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (ink(bitmap, x, y))
        return true;
  return false;
}
static bool packed(const uint8_t *bits, int i) { return bits[i / 8] >> (i % 8) & 1; }
static bool icon_at(DfIcon icon) {
  for (int r = 0; r < 9; r++)
    for (int c = 0; c < 9; c++)
      if (ink(bitmap, 91 + c, 2 + r) != packed(df_icons[icon], r * 9 + c))
        return false;
  return true;
}
static bool bubble_matches_room(void) {
  for (int y = 0; y <= 13; y++)
    for (int x = 85; x <= 102; x++)
      if (ink(bitmap, x, y) != ink(base(), x, y))
        return false;
  return true;
}
/* Render the expected digits independently from the glyph table. */
static bool text_at(int cx, int top, const char *text) {
  int width = -1;
  for (const char *c = text; *c; c++)
    width += (*c == ':' ? 1 : 4) + 1;
  int x = cx - width / 2;
  for (const char *c = text; *c; c++) {
    int index = *c == ':' ? 10 : *c == '%' ? 11 : *c - '0', w = index == 10 ? 1 : 4;
    for (int r = 0; r < 7; r++)
      for (int k = 0; k < w; k++)
        if (ink(bitmap, x + k, top + r) != packed(df_digits[index], r * w + k))
          return false;
    x += w + 1;
  }
  return true;
}
static void put32(uint8_t *b, uint32_t v) {
  for (int i = 0; i < 4; i++)
    b[i] = v >> (8 * i);
}
/* Timeline entries: {time, wish}; wish 0 none, 1 sleeping, 2 hungry, 3 sleepy, 4 bored. */
static void set_hint(unsigned count, const uint32_t *at, const uint8_t *wish) {
  memcpy(hint, "DFH2", 4);
  for (unsigned i = 0; i < 8; i++) {
    put32(hint + 4 + i * 5, i < count ? at[i] : UINT32_MAX);
    hint[8 + i * 5] = i < count ? wish[i] : 0;
  }
  hint_present = true;
  frame_tick = 0; /* force a re-read on the next update */
}
static void draw(Canvas *canvas) {
  unsigned before = sd_calls;
  drawing = true;
  deepflipper_render_reset();
  deepflipper_draw(canvas);
  drawing = false;
  assert(deepflipper_rendered() && sd_calls == before && !mutex.held);
  assert(memcmp(canvas->pixels, bitmap, 1024) == 0);
}
static void preview(const char *directory, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/home-%s.draw", directory, name);
  FILE *out = fopen(path, "wb");
  assert(out && fwrite(bitmap, 1, 1024, out) == 1024);
  fclose(out);
}
int main(int argc, char **argv) {
  assert(argc == 3);
  FILE *input = fopen(argv[1], "rb");
  assert(input);
  fseek(input, 0, SEEK_END);
  atlas_size = ftell(input);
  rewind(input);
  assert(atlas_size == DF_FRAME_COUNT * DF_HOME_FRAME_SIZE * DF_HOME_BANKS);
  atlas = malloc(atlas_size);
  assert(atlas && fread(atlas, 1, atlas_size, input) == atlas_size);
  fclose(input);
  deepflipper_sprite_init();
  assert(memcmp(bitmap, df_fallback, 1024) == 0);
  snapshot = (DeepFlipperSnapshot){.enabled = true,
                                   .connected = true,
                                   .microphone = true,
                                   .volume = 75,
                                   .battery = 86,
                                   .state = DeepFlipperIdle};
  for (unsigned bank = 0; bank < 2; bank++) {
    date.hour = bank ? 22 : 12;
    for (unsigned emotion = 0; emotion < 12; emotion++) {
      snapshot.emotion = emotion;
      update();
      assert(offset / 1024 >= bank * DF_FRAME_COUNT &&
             offset / 1024 < (bank + 1) * DF_FRAME_COUNT);
      outside_overlays_match();
    }
  }
  Canvas canvas;
  /* Time on the alarm clock, battery on the rice cooker, 2 px margins. */
  date.hour = 12;
  snapshot.emotion = 0;
  update();
  draw(&canvas);
  assert(text_at(16, 26, "12:34") && text_at(113, 29, "86%"));
  assert(!any_ink(4, 24, 5, 34) && !any_ink(27, 24, 28, 34));
  assert(bubble_matches_room());
  preview(argv[2], "ready");
  snapshot.battery = 100;
  update();
  assert(text_at(113, 29, "100%") && !any_ink(101, 28, 102, 37) &&
         !any_ink(123, 28, 124, 37));
  snapshot.battery = 7;
  update();
  assert(text_at(113, 29, "7%"));
  snapshot.battery = 86;
  date.hour = 22;
  update();
  draw(&canvas);
  assert(text_at(16, 26, "22:34"));
  preview(argv[2], "night");
  for (unsigned hour = 0; hour < 24; hour++) {
    date.hour = hour;
    update();
    assert(offset / 1024 / DF_FRAME_COUNT == (hour >= 20 || hour < 7));
  }
  time_format = LocaleTimeFormat12h;
  date.hour = 0;
  update();
  assert(text_at(16, 26, "12:34"));
  date.hour = 15;
  date.minute = 5;
  update();
  assert(text_at(16, 26, "03:05"));
  time_format = LocaleTimeFormat24h;
  date.hour = 12;
  date.minute = 34;
  /* Charging: steam above the cooker, nowhere else, and it moves. */
  snapshot.charging = true;
  update();
  assert(any_ink(107, 11, 115, 15));
  uint8_t steam[1024];
  memcpy(steam, bitmap, 1024);
  for (unsigned i = 0; i < 4; i++)
    update();
  assert(memcmp(steam, bitmap, 1024) != 0);
  outside_overlays_match();
  snapshot.charging = false;
  /* Foreground apps own the screen: no SD access at all. */
  unsigned before = sd_calls;
  snapshot.foreground = true;
  update();
  assert(sd_calls == before);
  snapshot.foreground = false;
  for (unsigned failure = 0; failure < 4; failure++) {
    missing = failure == 0;
    bad_size = failure == 1;
    seek_error = failure == 2;
    short_read = failure == 3;
    update();
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 128; x++)
        if (!overlay(x, y))
          assert(ink(bitmap, x, y) == ink(df_fallback, x, y));
    assert(text_at(16, 26, "12:34"));
  }
  draw(&canvas);
  preview(argv[2], "fallback");
  missing = bad_size = seek_error = short_read = false;
  /* Live mouth stays bounded to its 5x4 box. */
  snapshot.state = DeepFlipperSpeaking;
  for (unsigned mouth = 0; mouth < 3; mouth++) {
    snapshot.mouth = mouth;
    update();
    for (unsigned y = 33; y < 37; y++)
      for (unsigned x = 58; x < 63; x++)
        assert(ink(bitmap, x, y) == (df_mouth[mouth][y - 33] >> (x - 58) & 1));
    assert(icon_at(DfIconWave));
  }
  /* Assistant states become small bubble icons near her head. */
  snapshot.state = DeepFlipperListening;
  update();
  assert(icon_at(DfIconMic));
  draw(&canvas);
  preview(argv[2], "listening");
  snapshot.state = DeepFlipperThinking;
  update();
  assert(icon_at(DfIconDots));
  snapshot.state = DeepFlipperSpeaking;
  snapshot.muted = true;
  update();
  assert(icon_at(DfIconMute) &&
         offset / 1024 % DF_FRAME_COUNT ==
             df_sequences[0][(ticks / 120) % df_sequence_sizes[0]]);
  snapshot.muted = false;
  snapshot.volume = 0;
  update();
  assert(icon_at(DfIconMute));
  snapshot.volume = 75;
  snapshot.state = DeepFlipperError;
  update();
  assert(icon_at(DfIconBang));
  snapshot.state = DeepFlipperIdle;
  snapshot.microphone = false;
  update();
  assert(icon_at(DfIconMute));
  snapshot.microphone = true;
  snapshot.connected = false;
  update();
  assert(icon_at(DfIconQuestion));
  snapshot.connected = true;
  update();
  assert(bubble_matches_room());
  /* Singing (emotion 12): while a song plays and while she practises; listening still wins. */
  snapshot.emotion = 12;
  for (unsigned i = 0; i < 2; i++) {
    snapshot.state = i ? DeepFlipperSpeaking : DeepFlipperIdle;
    update();
    assert(offset / 1024 % DF_FRAME_COUNT ==
           df_sequences[12][(ticks / 120) % df_sequence_sizes[12]]);
  }
  snapshot.state = DeepFlipperListening;
  update();
  assert(offset / 1024 % DF_FRAME_COUNT == df_sequences[1][(ticks / 120) % df_sequence_sizes[1]]);
  snapshot.state = DeepFlipperIdle;
  snapshot.emotion = 0;
  update();
  /* Level-up: a few seconds of the happy animation and a star bubble. */
  deepflipper_celebrate();
  update();
  assert(icon_at(DfIconStar) &&
         offset / 1024 % DF_FRAME_COUNT ==
             df_sequences[4][(ticks / 120) % df_sequence_sizes[4]]);
  snapshot.state = DeepFlipperListening;
  update();
  assert(icon_at(DfIconMic)); /* the assistant still wins */
  snapshot.state = DeepFlipperIdle;
  for (unsigned i = 0; i < 34; i++)
    update();
  assert(bubble_matches_room() &&
         offset / 1024 % DF_FRAME_COUNT ==
             df_sequences[0][(ticks / 120) % df_sequence_sizes[0]]);
  /* Pet timeline: read about every 5 s, never by the GUI. */
  uint32_t now = 1000000 + 12 * 3600 + 34 * 60;
  {
    const uint32_t at[] = {now - 600, now - 1, now + 600};
    const uint8_t wish[] = {0, 2, 3};
    set_hint(3, at, wish);
  }
  ticks = (ticks / 120 / 60 + 1) * 60 * 120 - 120; /* next update: tick%60==0 */
  frame_tick = 0;
  update();
  assert(icon_at(DfIconRice));
  unsigned reads = hint_reads;
  for (unsigned i = 0; i < 39; i++)
    update();
  assert(hint_reads - reads <= 1); /* about every 40 frames (~5 s) */
  update(); /* tick % 60 == 40: the wish bubble blinks off */
  assert(bubble_matches_room());
  preview(argv[2], "hungry-off");
  /* An assistant state always wins over the pet's wish. */
  snapshot.state = DeepFlipperListening;
  ticks = (ticks / 120 / 60 + 1) * 60 * 120 - 120;
  update();
  assert(icon_at(DfIconMic));
  snapshot.state = DeepFlipperIdle;
  {
    const uint32_t at[] = {now - 5, now};
    const uint8_t wish[] = {2, 3};
    set_hint(2, at, wish);
  }
  update();
  assert(icon_at(DfIconSleep));
  {
    const uint32_t at[] = {now};
    const uint8_t wish[] = {4};
    set_hint(1, at, wish);
  }
  update();
  assert(icon_at(DfIconBall));
  {
    const uint32_t at[] = {now - 5, now + 1};
    const uint8_t wish[] = {0, 2};
    set_hint(2, at, wish);
  }
  update();
  assert(bubble_matches_room());
  /* Sleeping pet: sleeping animation and a Zzz bubble until she wakes. */
  {
    const uint32_t at[] = {now - 100, now + 1};
    const uint8_t wish[] = {1, 0};
    set_hint(2, at, wish);
  }
  update();
  assert(icon_at(DfIconSleep));
  assert(offset / 1024 % DF_FRAME_COUNT ==
         df_sequences[10][(ticks / 120) % df_sequence_sizes[10]]);
  draw(&canvas);
  preview(argv[2], "pet-sleeping");
  {
    const uint32_t at[] = {now - 100, now};
    const uint8_t wish[] = {1, 0};
    set_hint(2, at, wish);
  }
  update();
  assert(bubble_matches_room() &&
         offset / 1024 % DF_FRAME_COUNT ==
             df_sequences[0][(ticks / 120) % df_sequence_sizes[0]]);
  /* The real app encoder and the firmware reader agree end to end. */
  {
    WhalePet real;
    WhalePetHint timeline;
    whale_pet_init(&real, now - 17 * 600);
    whale_pet_forecast(&real, &timeline);
    whale_pet_hint_encode(&timeline, hint);
    hint_present = true;
    frame_tick = 0;
    update();
    assert(icon_at(DfIconRice)); /* 72 fullness is below 40 after 17 steps */
    real.sleeping = true;
    real.updated = now;
    whale_pet_forecast(&real, &timeline);
    whale_pet_hint_encode(&timeline, hint);
    frame_tick = 0;
    update();
    assert(icon_at(DfIconSleep) &&
           offset / 1024 % DF_FRAME_COUNT ==
               df_sequences[10][(ticks / 120) % df_sequence_sizes[10]]);
  }
  /* Corrupt or missing timeline: nothing shown. */
  {
    const uint32_t at[] = {now - 100};
    const uint8_t wish[] = {2};
    set_hint(1, at, wish);
  }
  memcpy(hint, "XXXX", 4);
  update();
  assert(bubble_matches_room());
  set_hint(1, (const uint32_t[]){now - 100}, (const uint8_t[]){9});
  update();
  assert(bubble_matches_room());
  hint_present = false;
  frame_tick = 0;
  update();
  assert(bubble_matches_room());
  /* The lock is shown as a bubble, even over an assistant state. */
  snapshot.state = DeepFlipperListening;
  locked = true;
  update();
  assert(icon_at(DfIconLock));
  snapshot.state = DeepFlipperIdle;
  snapshot.enabled = false;
  update();
  assert(icon_at(DfIconLock));
  draw(&canvas);
  preview(argv[2], "locked");
  free(atlas);
  puts("PASS: complete desktop renderer, day/night/emotions, clock and rice-cooker "
       "battery in the room, status/pet bubbles, pet sleep sync, charging steam, "
       "lock, level-up cheer, bounded live mouth, missing/corrupt SD fallback and SD-free GUI draws");
}
