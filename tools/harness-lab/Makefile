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

SRCS      := $(wildcard $(CURDIR)/src/*.cpp) \
             $(wildcard $(REPO)/lib/harness/src/*.cpp) \
             $(wildcard $(REPO)/lib/harness/src/providers/*.cpp) \
             $(wildcard $(REPO)/lib/core/src/orch_*.cpp) \
             $(REPO)/lib/core/src/attention.cpp \
             $(REPO)/lib/core/src/status_style.cpp \
             $(REPO)/lib/core/src/docs_pack.cpp \
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
