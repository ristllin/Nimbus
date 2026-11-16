# fold_replay - the offline owner-regression fold replay, built against nimbusd.
#
# nimbusd's engine and store objects are built first (make -C nimbusd check) and
# linked as they are; this adds one translation unit. Offline: no network, no keys.
#
#   make            build ./build/fold_replay
#   make clean
HERE    := $(abspath $(CURDIR))
REPO    := $(abspath $(HERE)/../../../..)
ND      := $(REPO)/nimbusd
BUILD   := $(HERE)/build
BIN     := $(BUILD)/fold_replay

AJ_SRC  := $(firstword $(wildcard $(REPO)/.pio/libdeps/native/ArduinoJson/src) \
                       $(wildcard $(REPO)/.pio/libdeps/esp32s3/ArduinoJson/src))
INCLUDES := -I$(ND)/src -I$(ND)/tests -I$(REPO)/lib/harness/include -I$(REPO)/lib/core/include \
            -I$(REPO)/include -I$(REPO)/../solide-drivers/include -I$(AJ_SRC)
CURL_CFLAGS := $(shell curl-config --cflags 2>/dev/null)
CURL_LIBS   := $(shell curl-config --libs 2>/dev/null || echo -lcurl)
# A non-system libcurl (miniconda/homebrew) needs its dir on the runtime search path.
CURL_PREFIX := $(shell curl-config --prefix 2>/dev/null)
ifneq ($(CURL_PREFIX),)
CURL_LIBS   += -Wl,-rpath,$(CURL_PREFIX)/lib
endif
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wno-unused-parameter -DNIMBUS_NATIVE $(CURL_CFLAGS) -MMD -MP $(INCLUDES)

.PHONY: all nimbusd clean relink
all: $(BIN)

nimbusd:
	@$(MAKE) -s -C $(ND) check >/dev/null

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/fold_replay.o: $(HERE)/fold_replay.cpp | nimbusd $(BUILD)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Always relinked after nimbusd is brought up to date, so a change under lib/ reaches it.
$(BIN): $(BUILD)/fold_replay.o relink
	@$(MAKE) -s -C $(ND) check >/dev/null
	@$(CXX) $(BUILD)/fold_replay.o $(wildcard $(ND)/build/core/*.o) $(CURL_LIBS) -lpthread -o $@
	@echo "built $@"

clean:
	@rm -f $(BUILD)/*.o $(BUILD)/*.d $(BIN)

-include $(wildcard $(BUILD)/*.d)
