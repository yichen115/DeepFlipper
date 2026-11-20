/* Record the production home renderer for README GIFs (not a test).
 * Prepended at build time with the stub prefix of tests/home_sprite_test.c. */

static FILE *record;

static void frame(unsigned repeat) {
  for (unsigned i = 0; i < repeat; i++) {
    ticks += 120;
    deepflipper_sprite_update(&snapshot);
    Canvas canvas;
    deepflipper_render_reset();
    deepflipper_draw(&canvas);
    fwrite(canvas.pixels, 1, 1024, record);
  }
}

static void timeline(uint32_t now, uint8_t wish, uint32_t until) {
  /* Same DFH2 layout the pet app publishes. */
  memset(hint, 0, sizeof(hint));
  memcpy(hint, "DFH2", 4);
  const uint32_t at[2] = {now - 60, until};
  const uint8_t wishes[2] = {wish, 0};
  for (unsigned i = 0; i < 8; i++) {
    uint32_t value = i < 2 ? at[i] : UINT32_MAX;
    for (unsigned b = 0; b < 4; b++)
      hint[4 + i * 5 + b] = value >> (8 * b);
    hint[8 + i * 5] = i < 2 ? wishes[i] : 0;
  }
  hint_present = true;
  frame_tick = 0;
}

int main(int argc, char **argv) {
  assert(argc == 3);
  FILE *input = fopen(argv[1], "rb");
  assert(input);
  fseek(input, 0, SEEK_END);
  atlas_size = ftell(input);
  rewind(input);
  atlas = malloc(atlas_size);
  assert(atlas && fread(atlas, 1, atlas_size, input) == atlas_size);
  fclose(input);
  record = fopen(argv[2], "wb");
  assert(record);
  deepflipper_sprite_init();
  snapshot = (DeepFlipperSnapshot){.enabled = true, .connected = true, .microphone = true,
                                   .volume = 75, .battery = 86, .state = DeepFlipperIdle};
  date.hour = 12;
  date.minute = 34;
  frame(36); /* idle in the day room */
  snapshot.state = DeepFlipperListening;
  frame(22);
  snapshot.state = DeepFlipperThinking;
  frame(18);
  snapshot.state = DeepFlipperSpeaking;
  for (unsigned i = 0; i < 30; i++) {
    snapshot.mouth = (i * 7 + i / 3) % 3;
    frame(1);
  }
  snapshot.state = DeepFlipperIdle;
  snapshot.emotion = 4; /* happy */
  frame(16);
  snapshot.emotion = 0;
  date.minute = 35;
  uint32_t now = 1000000 + 12 * 3600 + 35 * 60;
  timeline(now, 2, now + 100000); /* hungry: rice bowl bubble */
  ticks = (ticks / 120 / 60 + 1) * 60 * 120 - 120;
  frame(40);
  hint_present = false;
  frame_tick = 0;
  date.hour = 22;
  date.minute = 40;
  snapshot.battery = 63;
  snapshot.charging = true;
  now = 1000000 + 22 * 3600 + 40 * 60;
  timeline(now, 1, now + 100000); /* the pet sleeps at night */
  ticks = (ticks / 120 / 60 + 1) * 60 * 120 - 120;
  frame(44);
  fclose(record);
  free(atlas);
  return 0;
}
