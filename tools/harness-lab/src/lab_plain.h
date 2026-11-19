#pragma once

// lab_plain - the harness lab on engine 2 (the plain loop), lane RB-I1a. `nimbus-lab
// --engine=2 selftest` runs one scripted owner text through nimbusd's plain rig (the same
// header nimbusd uses) and the failure faces: a provider that errors, one that hangs past
// the wall clock, and an answer with nothing in it. No keys, no network: a scripted model
// at the transport, a virtual clock, storage in RAM. `make check` runs it beside the old
// engine's selftest, which --engine (default 1) leaves exactly as it was.
namespace lab {
int cmdPlainSelftest();
}
