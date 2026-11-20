/* Exercise the real view -> animation -> scene path, including warnings. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define furi_assert assert
#define EXT_PATH(path) "/ext/" path
#define RECORD_LOADER "loader"
#define RECORD_DOLPHIN "dolphin"
#define LOADER_APPLICATIONS_NAME "Applications"
#include "firmware/flipper/applications/services/deepflipper/deepflipper.h"
#include "firmware/flipper/applications/services/desktop/views/desktop_events.h"

enum {
  InputKeyOk,
  InputKeyBack,
  InputKeyUp,
  InputKeyDown,
  InputKeyLeft,
  InputKeyRight
};
enum {
  InputTypePress,
  InputTypeRelease,
  InputTypeShort,
  InputTypeLong,
  InputTypeRepeat
};
enum { FuriHalRtcFlagDebug, DesktopSceneLockMenu, DesktopSceneDebug };
enum {
  FavoriteAppLeftShort,
  FavoriteAppLeftLong,
  FavoriteAppRightShort,
  FavoriteAppRightLong,
  FavoriteAppOkLong,
  FavoriteCount
};
enum {
  DummyAppLeftShort,
  DummyAppDownShort,
  DummyAppOkShort,
  DummyAppRightShort,
  DummyAppUpLong,
  DummyAppDownLong,
  DummyAppLeftLong,
  DummyAppRightLong,
  DummyAppOkLong,
  DummyCount
};
enum {
  SceneManagerEventTypeCustom,
  SceneManagerEventTypeTick,
  AnimationManagerStateIdle,
  AnimationManagerStateBlocked
};
typedef struct {
  int key, type;
} InputEvent;
typedef struct {
  int type;
  uint32_t event;
} SceneManagerEvent;
typedef struct {
  char name_or_path[80];
} FavoriteApp;
typedef struct {
  bool levelup_pending;
  int state;
} AnimationManager;
typedef struct {
  int unused;
} Loader;
typedef struct {
  int unused;
} Dolphin;
typedef struct {
  struct {
    bool dummy_mode;
    FavoriteApp favorite_apps[FavoriteCount], dummy_apps[DummyCount];
  } settings;
  Loader *loader;
  AnimationManager *animation_manager;
  void *scene_manager;
  void *locked_view;
} Desktop;
typedef struct {
  void (*callback)(DesktopEvent, void *);
  void *context;
  void *poweroff_timer;
  bool dummy_mode;
} DesktopMainView;
typedef struct {
  void (*interact_callback)(void *);
  void *interact_callback_context;
} BubbleAnimationView;

static DeepFlipperSnapshot snapshot;
static bool debug, still_blocked;
static unsigned menu_calls, talk_calls, launch_calls, locks, timer_starts,
    timer_stops;
static unsigned upgrades, activations, checks, celebrations;
static int next_scene;
static char launched[80], talk_value[16];
static Loader loader;
void deepflipper_snapshot(DeepFlipperSnapshot *out) { *out = snapshot; }
bool deepflipper_module_command(const char *field, const char *value) {
  assert(strcmp(field, "talk") == 0);
  ++talk_calls;
  strcpy(talk_value, value);
  return true;
}
static bool furi_hal_rtc_is_flag_set(int flag) {
  (void)flag;
  return debug;
}
static void *furi_record_open(const char *record) {
  (void)record;
  return &loader;
}
static void furi_record_close(const char *record) { (void)record; }
static void loader_show_menu(Loader *target) {
  assert(target == &loader);
  ++menu_calls;
}
static void loader_start_detached_with_gui_error(Loader *target,
                                                 const char *path,
                                                 const char *args) {
  assert(target == &loader);
  (void)args;
  ++launch_calls;
  strcpy(launched, path);
}
static void scene_manager_set_scene_state(void *manager, int scene, int state) {
  (void)manager;
  (void)scene;
  (void)state;
}
static void scene_manager_next_scene(void *manager, int scene) {
  (void)manager;
  next_scene = scene;
}
static void desktop_lock(Desktop *desktop) {
  (void)desktop;
  ++locks;
}
static void desktop_view_locked_update(void *view) { (void)view; }
static void furi_timer_start(void *timer, unsigned ms) {
  (void)timer;
  assert(ms == 1300);
  ++timer_starts;
}
static void furi_timer_stop(void *timer) {
  (void)timer;
  ++timer_stops;
}
static void
animation_manager_check_blocking_process(AnimationManager *manager) {
  (void)manager;
  ++checks;
}
static void animation_manager_new_idle_process(AnimationManager *manager) {
  (void)manager;
  ++checks;
}
static void deepflipper_celebrate(void) { ++celebrations; }
static void dolphin_upgrade_level(Dolphin *dolphin) {
  (void)dolphin;
  ++upgrades;
}
static bool animation_manager_check_blocking(AnimationManager *manager) {
  (void)manager;
  return still_blocked;
}
static void animation_manager_start_new_idle(AnimationManager *manager) {
  manager->state = AnimationManagerStateIdle;
}
static void bubble_animation_activate(BubbleAnimationView *view, bool force) {
  (void)view;
  assert(!force);
  ++activations;
}
#define DESKTOP_MAIN_VIEW_POWEROFF_TIMEOUT 1300
#include "desktop-animation-interact-production.inc"
#include "desktop-bubble-input-production.inc"
#include "desktop-favorites-production.inc"
#include "desktop-input-production.inc"
#include "desktop-scene-production.inc"

static void dispatch(DesktopEvent event, void *context) {
  // Original Dummy Mode cases perform their action but return false.
  desktop_scene_main_on_event(
      context, (SceneManagerEvent){SceneManagerEventTypeCustom, event});
}
static void interact(void *context) {
  dispatch(DesktopAnimationEventInteractAnimation, context);
}
static void key(DesktopMainView *view, int key, int type) {
  InputEvent event = {key, type};
  BubbleAnimationView animation = {interact, view->context};
  if (!bubble_animation_input_callback(&event, &animation))
    assert(desktop_main_input_callback(&event, view));
}

int main(void) {
  AnimationManager manager = {.state = AnimationManagerStateIdle};
  Desktop desktop = {.loader = &loader, .animation_manager = &manager};
  DesktopMainView view = {.callback = dispatch, .context = &desktop};
  snapshot.enabled = true;
  debug = true;
  key(&view, InputKeyOk, InputTypeShort);
  assert(talk_calls == 1 && !menu_calls && strcmp(talk_value, "toggle") == 0);
  key(&view, InputKeyOk, InputTypeLong);
  assert(menu_calls == 1 &&
         talk_calls == 1); // Debug mode cannot steal the AI menu.
  key(&view, InputKeyRight, InputTypePress);
  key(&view, InputKeyRight, InputTypeRepeat);
  key(&view, InputKeyRight, InputTypeRelease);
  assert(!launch_calls && activations == 1);
  key(&view, InputKeyRight, InputTypeShort);
  assert(launch_calls == 1 &&
         strcmp(launched, "/ext/apps/Games/whale_pet.fap") == 0);

  manager.state = AnimationManagerStateBlocked;
  still_blocked = true;
  key(&view, InputKeyRight, InputTypeShort);
  assert(launch_calls == 1 && manager.state == AnimationManagerStateBlocked);
  still_blocked = false;
  key(&view, InputKeyRight, InputTypeShort);
  assert(launch_calls == 1 && manager.state == AnimationManagerStateIdle);
  manager.levelup_pending = true;
  manager.state = AnimationManagerStateBlocked;
  key(&view, InputKeyRight, InputTypeShort);
  /* Level-up is acknowledged once; the whale girl cheers on the home screen
     (no dolphin one-shot film), and the next Right opens the pet again. */
  assert(launch_calls == 1 && upgrades == 1 && celebrations == 1 &&
         !manager.levelup_pending && manager.state == AnimationManagerStateIdle);
  key(&view, InputKeyRight, InputTypeShort);
  assert(launch_calls == 2);

  strcpy(desktop.settings.favorite_apps[FavoriteAppRightLong].name_or_path,
         "Right-long");
  key(&view, InputKeyRight, InputTypeLong);
  assert(strcmp(launched, "Right-long") == 0);
  strcpy(desktop.settings.favorite_apps[FavoriteAppLeftShort].name_or_path,
         "Left-short");
  key(&view, InputKeyLeft, InputTypeShort);
  assert(strcmp(launched, "Left-short") == 0);
  key(&view, InputKeyUp, InputTypeShort);
  assert(next_scene == DesktopSceneLockMenu);
  key(&view, InputKeyUp, InputTypeLong);
  assert(locks == 1);
  key(&view, InputKeyDown, InputTypeLong);
  assert(next_scene == DesktopSceneDebug);
  key(&view, InputKeyDown, InputTypeShort);
  snapshot.connected = true;
  snapshot.state = DeepFlipperListening;
  key(&view, InputKeyBack, InputTypePress);
  key(&view, InputKeyBack, InputTypeRelease);
  key(&view, InputKeyBack, InputTypeShort);
  assert(timer_starts == 1 && timer_stops == 1 &&
         strcmp(talk_value, "cancel") == 0);

  snapshot.enabled = false;
  debug = false;
  key(&view, InputKeyOk, InputTypeShort);
  assert(menu_calls == 2);
  strcpy(desktop.settings.favorite_apps[FavoriteAppOkLong].name_or_path,
         "OK-long");
  key(&view, InputKeyOk, InputTypeLong);
  assert(strcmp(launched, "OK-long") == 0);
  debug = true;
  key(&view, InputKeyOk, InputTypeLong);
  assert(checks == 1);
  view.dummy_mode = desktop.settings.dummy_mode = true;
  key(&view, InputKeyRight, InputTypeShort);
  assert(strcmp(launched, "Passport") == 0);
  strcpy(desktop.settings.dummy_apps[DummyAppRightShort].name_or_path,
         "Dummy-right");
  key(&view, InputKeyRight, InputTypeShort);
  assert(strcmp(launched, "Dummy-right") == 0);
  key(&view, InputKeyOk, InputTypeShort);
  assert(strcmp(launched, "Passport") == 0 && menu_calls == 2);
  puts("PASS: real desktop input/scene path, AI talk/menu, pet shortcut, "
       "warning/level-up priority, original favorites and Dummy Mode");
}
