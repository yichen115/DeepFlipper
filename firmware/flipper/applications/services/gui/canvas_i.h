/**
 * @file canvas_i.h
 * GUI: internal Canvas API
 */

#pragma once

#include "canvas.h"
#include <u8g2.h>
#include <toolbox/compress.h>
#include <m-array.h>
#include <m-algo.h>
#include <furi.h>

#define ICON_DECOMPRESSOR_BUFFER_SIZE (128u * 64 / 8)

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CanvasCommitCallback)(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context);

typedef struct {
    CanvasCommitCallback callback;
    void* context;
} CanvasCallbackPair;

ARRAY_DEF(CanvasCallbackPairArray, CanvasCallbackPair, M_POD_OPLIST); //-V658

#define M_OPL_CanvasCallbackPairArray_t() ARRAY_OPLIST(CanvasCallbackPairArray, M_POD_OPLIST)

ALGO_DEF(CanvasCallbackPairArray, CanvasCallbackPairArray_t);

/** Canvas structure
 */
struct Canvas {
    u8g2_t fb;
    CanvasOrientation orientation;
    size_t offset_x;
    size_t offset_y;
    size_t width;
    size_t height;
    CompressIcon* compress_icon;
    CanvasCallbackPairArray_t canvas_callback_pair;
    FuriMutex* mutex;
};

/** Allocate memory and initialize canvas
 *
 * @return     Canvas instance
 */
Canvas* canvas_init(void);

/** Free canvas memory
 *
 * @param      canvas  Canvas instance
 */
void canvas_free(Canvas* canvas);

/** Set drawing region relative to real screen buffer
 *
 * @param      canvas    Canvas instance
 * @param      offset_x  x coordinate offset
 * @param      offset_y  y coordinate offset
 * @param      width     width
 * @param      height    height
 */
void canvas_frame_set(
    Canvas* canvas,
    int32_t offset_x,
    int32_t offset_y,
    size_t width,
    size_t height);

/** Draw a u8g2 bitmap
 *
 * @param      u8g2     u8g2 instance
 * @param      x        x coordinate
 * @param      y        y coordinate
 * @param      width    width
 * @param      height   height
 * @param      bitmap   bitmap
 * @param      rotation rotation
 */
void canvas_draw_u8g2_bitmap(
    u8g2_t* u8g2,
    int32_t x,
    int32_t y,
    size_t width,
    size_t height,
    const uint8_t* bitmap,
    IconRotation rotation);

/** Add canvas commit callback.
 *
 * This callback will be called upon Canvas commit.
 * 
 * @param      canvas    Canvas instance
 * @param      callback  CanvasCommitCallback
 * @param      context   CanvasCommitCallback context
 */
void canvas_add_framebuffer_callback(Canvas* canvas, CanvasCommitCallback callback, void* context);

/** Remove canvas commit callback.
 *
 * @param      canvas    Canvas instance
 * @param      callback  CanvasCommitCallback
 * @param      context   CanvasCommitCallback context
 */
void canvas_remove_framebuffer_callback(
    Canvas* canvas,
    CanvasCommitCallback callback,
    void* context);

/* DeepFlipper: on-demand capture of the text drawn in one complete frame, so the
 * assistant can read menus. Off unless armed; costs one pointer check per string. */
#define CANVAS_TEXT_ITEMS 24
#define CANVAS_TEXT_CHARS 27

typedef struct {
    int16_t x; /**< final left x after alignment */
    int16_t y; /**< baseline y */
    uint8_t color; /**< u8g2 draw color: 1 black, 0 white (inverted/highlighted), 2 xor */
    char text[CANVAS_TEXT_CHARS + 1];
} CanvasTextItem;

/** Arm capture of the next frame drawn between canvas_reset() and canvas_commit(). */
void canvas_text_capture_arm(void);

/** Copy the captured frame once complete. Returns false while not yet captured. */
bool canvas_text_capture_take(CanvasTextItem* items, size_t capacity, size_t* count);

/** Stop a pending capture. */
void canvas_text_capture_cancel(void);

#ifdef __cplusplus
}
#endif
