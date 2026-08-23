# nimbusd - the hosted Nimbus daemon (Virtual Nimbus, Phase 0).
#
# Like tools/harness-lab, this is a plain Makefile rather than a PlatformIO env:
# it links libcurl and produces real executables (the daemon + host tests),
# neither of which the `native` test platform does comfortably. The engine
# sources are exactly lib/core + lib/harness - the same portable code the
# firmware compiles - composed here with POSIX stores instead of SD/NVS.
#
#   make            build ./build/nimbusd
#   make test       build + run the offline (T1/T2) host tests -> the CI marker
#   make check      compile-only (no network) - the anti-rot gate
#   make clean
#
# ArduinoJson comes from whichever PlatformIO libdeps tree exists (populate it
# once with `pio test -e native` at the repo root, or copy a sibling's).

REPO      := $(abspath $(CURDIR)/..)
BUILD     := $(CURDIR)/build

AJ_SRC    := $(firstword $(wildcard $(REPO)/.pio/libdeps/native/ArduinoJson/src) \
                         $(wildcard $(REPO)/.pio/libdeps/esp32s3/ArduinoJson/src))

INCLUDES  := -I$(CURDIR)/src \
             -I$(CURDIR)/tests \
             -I$(REPO)/lib/harness/include \
             -I$(REPO)/lib/core/include \
             -I$(REPO)/include \
             -I$(REPO)/../solide-drivers/include \
             -I$(AJ_SRC)

# curl: system default on macOS; miniconda/homebrew otherwise. curl-config, when
# present, supplies the right -I/-L for a non-system libcurl.
CURL_CFLAGS := $(shell curl-config --cflags 2>/dev/null)
CURL_LIBS   := $(shell curl-config --libs 2>/dev/null || echo -lcurl)
# A non-system libcurl (miniconda/homebrew) needs its dir on the runtime search
# path or the binary aborts with "Library not loaded" at start.
CURL_PREFIX := $(shell curl-config --prefix 2>/dev/null)
ifneq ($(CURL_PREFIX),)
CURL_LIBS   += -Wl,-rpath,$(CURL_PREFIX)/lib
endif

# -MMD -MP is load-bearing (header-dep tracking): several composition headers are
# header-only, so without it an edit to one rebuilds only the .cpp that changed
# and the other TU keeps a stale inline definition (harness-lab paid for this).
CXXFLAGS  := -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
             -DNIMBUS_NATIVE $(CURL_CFLAGS) -MMD -MP $(INCLUDES)
LDFLAGS   := $(CURL_LIBS) -lpthread

# The portable engine + memory sources (identical to what harness-lab links).
CORE_SRCS := $(wildcard $(REPO)/lib/harness/src/*.cpp) \
             $(wildcard $(REPO)/lib/harness/src/providers/*.cpp) \
             $(wildcard $(REPO)/lib/core/src/orch_*.cpp) \
             $(REPO)/lib/core/src/attention.cpp \
             $(REPO)/lib/core/src/status_style.cpp \
             $(REPO)/lib/core/src/docs_pack.cpp \
             $(REPO)/lib/core/src/tg_updates.cpp \
             $(REPO)/lib/core/src/tg_html.cpp \
             $(wildcard $(REPO)/../solide-drivers/src/portable/ring.cpp)

CORE_OBJS := $(patsubst %,$(BUILD)/core/%.o,$(notdir $(basename $(CORE_SRCS))))
CORE_VPATH := $(sort $(dir $(CORE_SRCS)))

# Test binaries: one per tests/test_*.cpp.
TEST_SRCS := $(wildcard $(CURDIR)/tests/test_*.cpp)
TEST_BINS := $(patsubst $(CURDIR)/tests/%.cpp,$(BUILD)/%,$(TEST_SRCS))

BIN := $(BUILD)/nimbusd

.PHONY: all test check clean daemon
all: daemon

# The daemon binary.
daemon: $(BIN)
$(BIN): $(CURDIR)/src/main.cpp $(CORE_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(CORE_OBJS) $(LDFLAGS) -o $@
	@echo "built $@"

# The live scenario suite (PAID: runs real provider turns). Built by `make
# scenarios`, but never run automatically - `make test` stays offline/free.
SCN_BIN := $(BUILD)/nimbusd-scenarios
.PHONY: scenarios
scenarios: $(SCN_BIN)
$(SCN_BIN): $(CURDIR)/scenarios/scenarios.cpp $(CORE_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(CORE_OBJS) $(LDFLAGS) -o $@
	@echo "built $@ (run with provider keys in the env; PAID)"

vpath %.cpp $(CORE_VPATH)

$(BUILD)/core/%.o: %.cpp | $(BUILD)/core
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%: $(CURDIR)/tests/%.cpp $(CORE_OBJS) | $(BUILD)
	@$(CXX) $(CXXFLAGS) $< $(CORE_OBJS) $(LDFLAGS) -o $@

$(BUILD) $(BUILD)/core:
	@mkdir -p $@

# Run every test binary; fail the whole target if any returns non-zero.
test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do echo; echo ">>> $$t"; $$t || fail=1; done; \
	 echo; if [ $$fail -eq 0 ]; then echo "ALL NIMBUSD TESTS PASSED"; else echo "NIMBUSD TESTS FAILED"; fi; \
	 exit $$fail

# Compile-only: prove the daemon + store layer + tests still build against the
# current engine headers, no network or keys needed.
check: $(BIN) $(TEST_BINS)
	@echo "nimbusd + tests compile against the current engine"

-include $(CORE_OBJS:.o=.d)
-include $(TEST_BINS:=.d)
-include $(BIN:=.d)

clean:
	@rm -rf $(BUILD)
