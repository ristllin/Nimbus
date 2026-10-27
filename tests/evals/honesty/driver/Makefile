# hon_driver - the honesty eval's host driver, built against the harness lab.
#
# The lab (tools/harness-lab) is built first and its objects reused as they are.
# Two objects are swapped: main.o (the lab has its own main) and compose.o, which
# is rebuilt here with composeInstructions renamed to composeInstructionsReal so
# hon_driver.cpp can wrap it (it adds the clock, the firmware line and the hardware
# manifest that the lab's compose inputs leave out). Nothing else is swapped.
#
#   make            build ./build/hon_driver
#   make clean
HERE   := $(abspath $(CURDIR))
REPO   := $(abspath $(HERE)/../../../..)
LAB    := $(REPO)/tools/harness-lab
BUILD  := $(HERE)/build
BIN    := $(BUILD)/hon_driver

AJ_SRC := $(firstword $(wildcard $(REPO)/.pio/libdeps/native/ArduinoJson/src) \
                      $(wildcard $(REPO)/.pio/libdeps/esp32s3/ArduinoJson/src))
INCLUDES := -I$(LAB)/src -I$(REPO)/lib/harness/include -I$(REPO)/lib/core/include \
            -I$(REPO)/include -I$(REPO)/../solide-drivers/include -I$(AJ_SRC)
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wno-unused-function -DNIMBUS_NATIVE -MMD -MP $(INCLUDES)

.PHONY: all lab clean relink
all: $(BIN)

lab:
	@$(MAKE) -s -C $(LAB)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/compose_real.o: $(REPO)/lib/harness/src/compose.cpp | lab $(BUILD)
	@$(CXX) $(CXXFLAGS) -DcomposeInstructions=composeInstructionsReal -c $< -o $@

$(BUILD)/hon_driver.o: $(HERE)/hon_driver.cpp | lab $(BUILD)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Always relinked (a few seconds), after the lab is brought up to date, so a change under
# lib/ always reaches the binary: the lab objects are listed only once `lab` has run.
$(BIN): $(BUILD)/hon_driver.o $(BUILD)/compose_real.o relink
	@$(MAKE) -s -C $(LAB)
	@$(CXX) $(filter %.o,$^) $(filter-out $(LAB)/build/main.o $(LAB)/build/compose.o,$(wildcard $(LAB)/build/*.o)) -lcurl -o $@
	@echo "built $@"

clean:
	@rm -f $(BUILD)/*.o $(BUILD)/*.d $(BIN)

-include $(wildcard $(BUILD)/*.d)
