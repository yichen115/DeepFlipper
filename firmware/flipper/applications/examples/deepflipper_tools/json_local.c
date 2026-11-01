// Keep the adapter's JSON parser in its SD application; no new firmware API.
#pragma GCC diagnostic ignored "-Wredundant-decls"
#define JSON_MINIMAL 1
#include "../../../lib/mjs/common/frozen/frozen.c"
