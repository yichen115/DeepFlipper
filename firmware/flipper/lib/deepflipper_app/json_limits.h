#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/** Bound recursive JSON parsers before parsing untrusted application messages.
 * This is a size/depth check; the actual JSON parser still validates syntax. */
static inline bool
    deepflipper_json_within_limits(const char* text, size_t maximum_bytes, unsigned maximum_depth) {
    size_t size = strlen(text);
    if(size > maximum_bytes) return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for(size_t i = 0; i < size; ++i) {
        char c = text[i];
        if(quoted) {
            if(escaped)
                escaped = false;
            else if(c == '\\')
                escaped = true;
            else if(c == '"')
                quoted = false;
        } else if(c == '"')
            quoted = true;
        else if(c == '{' || c == '[') {
            if(++depth > maximum_depth) return false;
        } else if(c == '}' || c == ']') {
            if(!depth) return false;
            --depth;
        }
    }
    return !quoted && !depth;
}
