# harness-lab - build the Nimbus agent harness as a host executable.
#
# Deliberately a plain Makefile rather than a platformio env: this links libcurl
# and produces an executable, neither of which PlatformIO's `native` platform
# (built around Unity test runners) does comfortably. The sources are exactly
# lib/core + lib/harness - the same portable code the firmware compiles.
#
#   make            build ./build/nimbus-lab
#   make run        build, then run the default scenario set
#   make check      compile only (no network, no keys) - the anti-rot target
#   make clean

REPO      := $(abspath $(CURDIR)/../..)
BUILD     := $(CURDIR)/build
BIN       := $(BUILD)/nimbus-lab

# ArduinoJson comes from whichever PlatformIO libdeps tree exists (the native
# one is populated by `pio test -e native`).
AJ_SRC    := $(firstword $(wildcard $(REPO)/.pio/libdeps/native/ArduinoJson/src) \
                         $(wildcard $(REPO)/.pio/libdeps/esp32s3/ArduinoJson/src))

INCLUDES  := -I$(CURDIR)/src \
             -I$(REPO)/lib/harness/include \
             -I$(REPO)/lib/core/include \
             -I$(REPO)/include \
             -I$(REPO)/../solide-drivers/include \
             -I$(AJ_SRC)

# -MMD -MP is load-bearing, not hygiene: LabRig is header-only, so without header
# dependency tracking an edit to lab_rig.h rebuilds only the .cpp files that
# changed. The other TU keeps its stale inline definitions, the linker picks one
# arbitrarily, and you debug a fix that is genuinely in the source but not in the
# binary. That cost a real half-hour here.
CXXFLAGS  := -std=gnu++17 -O1 -g -Wall -Wno-unused-function -DNIMBUS_NATIVE -MMD -MP $(INCLUDES)
LDFLAGS   := -lcurl

# The lib/core portion cannot glob the whole directory: most of lib/core is
# firmware-facing (tft, ring, wifi, cloud relay) and pulls in driver/Arduino
# symbols this host link cannot resolve. So it is a curated set - the orchestrator
# core plus the handful of portable helpers the harness engine references.
#
# A curated list drifts silently: when lib/core grows a new source the engine
# needs (e.g. fallback_rules.cpp, the CUM-41 fallback engine, which is orchestrator
# code that does NOT carry the orch_ prefix), the link breaks with an undefined
# reference and nothing catches it - the lab is in no default sweep. The `check`
# target below is wired into tools/build_all.sh (AGENTS.md section 3) exactly so
# that drift fails loudly in the matrix instead of rotting until the next live run.
SRCS      := $(wildcard $(CURDIR)/src/*.cpp) \
             $(wildcard $(REPO)/lib/harness/src/*.cpp) \
             $(wildcard $(REPO)/lib/harness/src/providers/*.cpp) \
             $(wildcard $(REPO)/lib/core/src/orch_*.cpp) \
             $(REPO)/lib/core/src/attention.cpp \
             $(REPO)/lib/core/src/status_style.cpp \
             $(REPO)/lib/core/src/docs_pack.cpp \
             $(REPO)/lib/core/src/fallback_rules.cpp \
             $(REPO)/lib/core/src/model_catalog.cpp \
             $(wildcard $(REPO)/../solide-drivers/src/portable/ring.cpp)

OBJS      := $(patsubst %,$(BUILD)/%.o,$(notdir $(basename $(SRCS))))
VPATH     := $(sort $(dir $(SRCS)))

.PHONY: all run check clean
all: $(BIN)

$(BIN): $(OBJS) | $(BUILD)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@
	@echo "built $@"

$(BUILD)/%.o: %.cpp | $(BUILD)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

-include $(OBJS:.o=.d)

run: $(BIN)
	$(BIN) scenarios

# Compile-only gate: proves the lab still builds against the current harness
# without needing keys or a network. Cheap enough to run in any sweep.
check: $(BIN)
	@echo "harness-lab compiles against the current harness"

clean:
	@rm -rf $(BUILD)
