# provider-live - real-API legs for the provider wire (lane K): the device's own
# adapters, fabric loop and cap table over libcurl, driven by small scripted
# scenarios with deterministic fake tools, so a leg costs cents and asserts the
# wire rule it exists for (never only "the API returned 200").
#
#   make            build ./build/provider-live
#   make check      compile only (no network, no keys)
#
# Keys are read by NAME from NIMBUS_ENV_FILE (the secrets .env); nothing is
# committed or printed.

REPO      := $(abspath $(CURDIR)/../..)
BUILD     := $(CURDIR)/build
BIN       := $(BUILD)/provider-live
AJ_SRC    := $(firstword $(wildcard $(REPO)/.pio/libdeps/native/ArduinoJson/src) \
                         $(wildcard $(REPO)/.pio/libdeps/esp32s3/ArduinoJson/src))
INCLUDES  := -I$(REPO)/lib/harness/include -I$(REPO)/lib/core/include -I$(REPO)/include \
             -I$(REPO)/../solide-drivers/include -I$(AJ_SRC)
CXXFLAGS  := -std=gnu++17 -O1 -g -Wall -Wno-unused-function -DNIMBUS_NATIVE -MMD -MP $(INCLUDES)
LDFLAGS   := -lcurl
SRCS      := $(CURDIR)/main.cpp \
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

.PHONY: all check clean
all: $(BIN)
$(BIN): $(OBJS) | $(BUILD)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@
$(BUILD)/%.o: %.cpp | $(BUILD)
	@$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD):
	@mkdir -p $(BUILD)
-include $(OBJS:.o=.d)
check: $(BIN)
	@echo "provider-live compiles against the current harness"
clean:
	@rm -rf $(BUILD)
