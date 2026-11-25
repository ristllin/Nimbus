#pragma once

// log_console - the ESP-IDF log's console writer (lane E2-STACK2, CUM-997).
//
// esp_log formats each line through a vprintf hook. The default, vprintf on the
// unbuffered stdout, runs newlib's _vfprintf_r twice (once more through __sbprintf's
// buffered stand-in): about 3 KB of stack under whatever task logged. A driver logs
// from deep inside a durable log write (the SD card's write error, sdmmc_cmd.c), so
// that cost landed at the bottom of the Telegram task's deepest chain.
//
// install() replaces it with a formatter into a small stack buffer (vsnprintf, one
// _svfprintf_r, no stream) and one write() of the whole line to stdout's descriptor:
// the same bytes, in one piece as before. A line longer than the buffer is formatted
// again into PSRAM; if that allocation fails, the part that fit goes out with a true
// marker saying the rest was cut. Call once in setup(), before any task logs.
namespace nimbus::logconsole {

void install();

}  // namespace nimbus::logconsole
