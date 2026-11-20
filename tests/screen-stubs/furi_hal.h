/* Host boundary for tests/screen_capture_test.c. */
#pragma once
#include <stdbool.h>
#define FuriHalRtcFlagLock 1
bool furi_hal_rtc_is_flag_set(int flag);
