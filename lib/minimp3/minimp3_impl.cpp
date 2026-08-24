// minimp3 implementation TU. The single-header decoder (minimp3.h) is CC0 /
// public domain (lieff/minimp3); this one translation unit pulls in its code so
// every other file includes only the declarations. Kept in lib/ so PlatformIO's
// LDF compiles it whenever music.cpp includes "minimp3.h" - no platformio.ini
// change needed. Fixed-point output (no MINIMP3_FLOAT_OUTPUT) => int16 PCM.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"
