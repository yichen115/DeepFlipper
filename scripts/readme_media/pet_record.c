/* Record the production Whale Pet screens for README GIFs (not a test).
 * Prepended at build time with the stub prefix of tests/pet_render_test.c. */

static FILE *record;
static uint32_t clock_ms = 1000;

static void capture(PetApp *app) {
  PetClip clip = pet_ui_clip(&app->ui);
  show(app, clip, pet_ui_frame(&app->ui, clock_ms));
  app->now = clock_ms;
  Canvas canvas = {0};
  draw(&canvas, app);
  fwrite(canvas.px, 1, sizeof(canvas.px), record);
}

/* Advance the controller like run_ui (50 ms ticks) and grab a frame every 100 ms. */
static void play(PetApp *app, unsigned frames) {
  for (unsigned i = 0; i < frames; i++) {
    for (unsigned t = 0; t < 2; t++) {
      clock_ms += 50;
      PetCommand command = pet_ui_tick(&app->ui, &app->store.pet, 14, clock_ms);
      if (command.type == PetCommandGameOver) {
        WhalePet next = app->store.pet;
        unsigned level = whale_pet_level(&next);
        WhalePetResult result = whale_pet_game(&next, command.score, 600000);
        app->store.pet = next;
        pet_ui_game_scored(&app->ui, &app->store.pet, result, true, level, clock_ms);
      }
    }
    capture(app);
  }
}

static void press(PetApp *app, PetKey key, PetPress kind) {
  PetCommand command = pet_ui_input(&app->ui, &app->store.pet, key, kind, clock_ms);
  if (command.type == PetCommandGameOver) {
    WhalePet next = app->store.pet;
    unsigned level = whale_pet_level(&next);
    WhalePetResult result = whale_pet_game(&next, command.score, 600000);
    app->store.pet = next;
    pet_ui_game_scored(&app->ui, &app->store.pet, result, true, level, clock_ms);
  } else if (command.type == PetCommandCare) {
    WhalePet next = app->store.pet;
    unsigned level = whale_pet_level(&next);
    WhalePetResult result = whale_pet_apply(&next, command.action, 500000 + clock_ms / 1000);
    app->store.pet = next;
    pet_ui_cared(&app->ui, &app->store.pet, command.action, result, true, level, clock_ms);
  }
}

/* A simple player: steer the bowl under the lowest falling grain. */
static void play_game(PetApp *app, unsigned frames) {
  for (unsigned i = 0; i < frames && !app->ui.game.over; i++) {
    const PetGame *game = &app->ui.game;
    const PetDrop *target = NULL;
    for (unsigned d = 0; d < PetGameDrops; d++)
      if (game->drops[d].active && (!target || game->drops[d].y4 > target->y4))
        target = &game->drops[d];
    int centre = game->bowl_x + PetGameBowlW / 2;
    int goal = target ? target->x + (target->bonus ? 3 : 1) : centre;
    bool left = goal < centre - 2, right = goal > centre + 2;
    if (left != game->left) press(app, PetKeyLeft, left ? PetPressDown : PetPressUp);
    if (right != game->right) press(app, PetKeyRight, right ? PetPressDown : PetPressUp);
    play(app, 1);
  }
}

int main(int argc, char **argv) {
  assert(argc == 4);
  frames = load(argv[1]);
  masks = load(argv[2]);
  record = fopen(argv[3], "wb");
  assert(record);
  static PetApp app;
  app.save_ok = app.assets_ok = true;
  app.store.status = WhalePetStoreReady;
  whale_pet_init(&app.store.pet, 500000);
  app.store.pet.fullness = 34; /* a little hungry, so Feed is suggested */
  app.store.pet.xp = 40;
  app.store.pet.bond = 36;
  app.ui.random = 11;
  pet_ui_open(&app.ui, &app.store.pet, 7200, 14, clock_ms);
  play(&app, 26); /* wave + "Welcome back!" */
  play(&app, 8);
  press(&app, PetKeyOk, PetPressShort); /* Feed: white rice */
  play(&app, 50);
  play(&app, 6);
  press(&app, PetKeyRight, PetPressShort);
  play(&app, 5);
  press(&app, PetKeyRight, PetPressShort); /* Catch */
  play(&app, 7);
  press(&app, PetKeyOk, PetPressShort);
  play_game(&app, 170);
  press(&app, PetKeyLeft, PetPressUp);
  press(&app, PetKeyRight, PetPressUp);
  press(&app, PetKeyBack, PetPressShort); /* end the round, show the score */
  play(&app, 14);
  press(&app, PetKeyOk, PetPressShort);
  play(&app, 22);
  press(&app, PetKeyOk, PetPressLong); /* stats */
  play(&app, 14);
  press(&app, PetKeyRight, PetPressShort); /* expression gallery */
  play(&app, 12);
  press(&app, PetKeyRight, PetPressShort);
  play(&app, 12);
  fclose(record);
  free(frames);
  free(masks);
  return 0;
}
