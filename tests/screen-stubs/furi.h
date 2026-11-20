/* Host boundary for tests/screen_capture_test.c (not firmware code). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UNUSED(x)              (void)(x)
#define MIN(a, b)              ((a) < (b) ? (a) : (b))
#define furi_ms_to_ticks(ms)   (ms)
#define FURI_CRITICAL_ENTER()  do { } while(0)
#define FURI_CRITICAL_EXIT()   do { } while(0)

static inline size_t test_strlcpy(char* dst, const char* src, size_t size) {
    size_t len = strlen(src);
    if(size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}
#define strlcpy test_strlcpy

typedef struct {
    char text[64];
} FuriString;

uint32_t furi_get_tick(void);
void furi_delay_ms(uint32_t ms);
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
FuriString* furi_string_alloc(void);
void furi_string_free(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
