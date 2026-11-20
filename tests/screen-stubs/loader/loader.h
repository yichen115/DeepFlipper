/* Host boundary for tests/screen_capture_test.c. */
#pragma once
#include <stdbool.h>
#include <furi.h>
#define RECORD_LOADER "loader"
typedef struct Loader Loader;
bool loader_get_application_name(Loader* loader, FuriString* name);
