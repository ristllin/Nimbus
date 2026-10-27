# fc_driver - the food-coach eval's host driver, built against the harness lab.
#
# The lab (tools/harness-lab) is built first and its objects reused as they are.
# Two objects are swapped: main.o (the lab has its own main) and compose.o, which
# is rebuilt here with composeInstructions renamed to composeInstructionsReal so
# fc_driver.cpp can define the name and wrap it (it adds the clock and the stored
# directive that the lab's compose inputs leave out). Nothing under tools/ or lib/
# is edited; a change to compose.cpp is picked up on the next build.
#
#   make            build ./build/fc_driver
#   make clean
HERE   := $(abspath $(CURDIR))
REPO   := $(abspath $(HERE)/../../../..)
LAB    := $(REPO)/tools/harness-lab
BUILD  := $(HERE)/build
BIN    := $(BUILD)/fc_driver

# The lab's own include set (its Makefile is the single source of the flags).
AJ_SRC := $(firstword $(wildcard $(REPO)/.pio/libdeps/native/ArduinoJson/src) \
                      $(wildcard $(REPO)/.pio/libdeps/esp32s3/ArduinoJson/src))
INCLUDES := -I$(LAB)/src -I$(REPO)/lib/harness/include -I$(REPO)/lib/core/include \
            -I$(REPO)/include -I$(REPO)/../solide-drivers/include -I$(AJ_SRC)
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wno-unused-function -DNIMBUS_NATIVE -MMD -MP $(INCLUDES)

LAB_OBJS := $(filter-out $(LAB)/build/main.o $(LAB)/build/compose.o,$(wildcard $(LAB)/build/*.o))

.PHONY: all lab clean
all: $(BIN)

lab:
	@$(MAKE) -s -C $(LAB)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/compose_real.o: $(REPO)/lib/harness/src/compose.cpp | lab $(BUILD)
	@$(CXX) $(CXXFLAGS) -DcomposeInstructions=composeInstructionsReal -c $< -o $@

$(BUILD)/fc_driver.o: $(HERE)/fc_driver.cpp | lab $(BUILD)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# The lab objects are listed by the shell when the link runs (after the lab built:
# a make function in the recipe is expanded before its first line runs, which left
# a fresh tree's first link without them), so a stale lab never links; the
# link always runs (FORCE), so a lab object rebuilt from a changed source (a playbook
# text, say) always reaches the binary.
.PHONY: FORCE
FORCE:
$(BIN): $(BUILD)/fc_driver.o $(BUILD)/compose_real.o FORCE
	@$(MAKE) -s -C $(LAB)
	@objs=$$(ls $(LAB)/build/*.o | grep -v -e '/main\.o$$' -e '/compose\.o$$'); \
	  $(CXX) $(filter-out FORCE,$^) $$objs -lcurl -o $@
	@echo "built $@"

clean:
	@rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
