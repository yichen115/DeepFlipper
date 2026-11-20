#pragma once
// Pure pet-screen controller: menu, reactions, bubbles and Catch Rice.
// No Furi/GUI/storage calls, so the production logic runs in host tests.
#include "pet_clips.h"
#include "pet_model.h"

typedef enum {
    PetPageMain,
    PetPageStats,
    PetPageFaces,
    PetPageGame,
} PetPage;

typedef enum {
    PetItemPat,
    PetItemFeed,
    PetItemPlay,
    PetItemCatch,
    PetItemSleep,
    PetItemStats,
    PetItemWake,
    PetItemCount,
} PetItem;

typedef enum {
    PetKeyOk,
    PetKeyBack,
    PetKeyUp,
    PetKeyDown,
    PetKeyLeft,
    PetKeyRight,
} PetKey;

typedef enum {
    PetPressShort,
    PetPressLong,
    PetPressDown, // key went down (game movement)
    PetPressUp, // key released
} PetPress;

typedef enum {
    PetCommandNone,
    PetCommandCare, // apply `action` to the model and save
    PetCommandGameOver, // apply the finished game score and save
    PetCommandExit,
} PetCommandType;

typedef struct {
    PetCommandType type;
    WhalePetAction action;
    unsigned score;
} PetCommand;

enum {
    PetGameWidth = 64, // left half of the screen is the play field
    PetGameBowlY = 56,
    PetGameBowlW = 17,
    PetGameDrops = 3,
    PetGameLives = 5,
    PetGameTickMs = 50,
    PetGameTicks = 600, // 30 s
    PetFlashFood = 1,
    PetFlashEnergy = 2,
    PetFlashJoy = 4,
};

typedef struct {
    int16_t y4; // top edge in quarter pixels (may start above the screen)
    int8_t x; // left edge in the play field
    uint8_t speed; // quarter pixels per tick
    bool bonus; // onigiri, worth 3
    bool active;
} PetDrop;

typedef struct {
    int8_t bowl_x;
    bool left, right; // held keys
    uint16_t score;
    uint8_t lives;
    uint16_t ticks;
    uint16_t spawn_in;
    uint32_t last_step;
    bool over;
    bool rewarded; // after PetCommandGameOver is acknowledged
    const char* result; // end panel line
    PetDrop drops[PetGameDrops];
} PetGame;

typedef struct {
    PetPage page;
    uint8_t menu[PetItemCount];
    uint8_t menu_count;
    uint8_t selected;
    bool menu_sleeping; // which menu is built
    PetClip clip;
    uint32_t clip_started;
    bool busy; // a care reaction is playing; the menu steps aside
    uint32_t next_idle;
    uint32_t last_nav; // last Left/Right on the menu
    char say[16];
    uint32_t say_until;
    uint8_t flash;
    uint8_t flash_decrease; // subset of flash: draw a minus for these actual decreases
    uint32_t flash_until;
    uint8_t pat_combo;
    uint32_t pat_last;
    uint8_t pending_level;
    uint8_t face; // gallery clip
    uint32_t random;
    PetGame game;
} PetUi;

// `away` is seconds since the save was last settled, `hour` is local RTC hour.
void pet_ui_open(PetUi* ui, const WhalePet* pet, uint32_t away, unsigned hour, uint32_t now_ms);
PetCommand pet_ui_input(PetUi* ui, const WhalePet* pet, PetKey key, PetPress press, uint32_t now_ms);
// Call after a PetCommandCare was executed; before is a snapshot from before applying it.
void pet_ui_cared(
    PetUi* ui,
    const WhalePet* pet,
    WhalePetAction action,
    WhalePetResult result,
    bool saved,
    const WhalePet* before,
    uint32_t now_ms);
// Call after a PetCommandGameOver was executed; before is the pre-operation snapshot.
void pet_ui_game_scored(
    PetUi* ui,
    const WhalePet* pet,
    WhalePetResult result,
    bool saved,
    const WhalePet* before,
    uint32_t now_ms);
// Advance timers. Returns a command (only GameOver) when the game ends.
PetCommand pet_ui_tick(PetUi* ui, const WhalePet* pet, unsigned hour, uint32_t now_ms);
// Current animation clip and frame index (the gallery shows `face`).
PetClip pet_ui_clip(const PetUi* ui);
unsigned pet_ui_frame(const PetUi* ui, uint32_t now_ms);
const char* pet_ui_item_label(PetItem item);
bool pet_ui_menu_visible(const PetUi* ui, uint32_t now_ms);
const char* pet_ui_bubble(const PetUi* ui, uint32_t now_ms);
