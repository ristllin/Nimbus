// Host builds: compile solide-drivers' portable ring compositor into nimbus-core
// so ring_plan/attention can be unit-tested natively. The device build gets the
// same TU from the solide-drivers library itself (see platformio.ini lib_deps),
// so this shim must stay empty there to avoid duplicate symbols.
#if defined(NIMBUS_NATIVE)
#include "../../../../solide-drivers/src/portable/ring.cpp"  // NOLINT
#endif
