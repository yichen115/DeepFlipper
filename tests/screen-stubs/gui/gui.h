/* Host boundary for tests/screen_capture_test.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define RECORD_GUI "gui"
typedef struct {
    int unused;
} Gui;
typedef int CanvasOrientation;
typedef void (*GuiCanvasCommitCallback)(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context);
void gui_add_framebuffer_callback(Gui* gui, GuiCanvasCommitCallback callback, void* context);
void gui_remove_framebuffer_callback(Gui* gui, GuiCanvasCommitCallback callback, void* context);
