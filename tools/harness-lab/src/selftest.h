#pragma once

// Offline checks of the lab's own seams (run pins, wire rewrite, recorder scrub,
// fault chain). No keys, no network; `make check` runs it.
namespace lab {
int cmdSelftest();
}
