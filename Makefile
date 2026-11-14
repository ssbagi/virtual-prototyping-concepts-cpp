# ============================================================================
# Author  : Shreyas S Bagi
# Note    : Developed with AI assistance (IBM Bob).
# Purpose : Created for learning and knowledge purposes only.
# ============================================================================
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
BUILD    := build
HEADERS  := logger.h arbiter.h bounded_buffer.h backpressure.h
TARGETS  := $(BUILD)/arbitration_starvation $(BUILD)/bounded_buffer_livelock $(BUILD)/backpressure_scenarios $(BUILD)/tests

LOGDIR   := log

.PHONY: all run test logs clean

all: $(TARGETS)

$(BUILD)/%: %.cc $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(BUILD)/arbitration_starvation $(BUILD)/bounded_buffer_livelock $(BUILD)/backpressure_scenarios
	@echo "################ 1) arbitration starvation ################"
	@$(BUILD)/arbitration_starvation
	@echo
	@echo "################ 2) bounded buffer livelock / head starvation ################"
	@$(BUILD)/bounded_buffer_livelock
	@echo
	@echo "################ 3) backpressure scenarios ################"
	@$(BUILD)/backpressure_scenarios

# Builds and runs the self-checking tests; exits non-zero on any failed check.
test: $(BUILD)/tests
	@$(BUILD)/tests

# Writes every demo at two verbosity levels plus the test run into $(LOGDIR)/.
#   *_high.log : narrated decisions of the first cycles (V_HIGH)
#   *_low.log  : results only (V_LOW), warnings and errors
logs: all | $(LOGDIR)
	{ $(BUILD)/arbitration_starvation  high; } > $(LOGDIR)/arbitration_starvation_high.log 2>&1
	{ $(BUILD)/arbitration_starvation  low;  } > $(LOGDIR)/arbitration_starvation_low.log  2>&1
	{ $(BUILD)/bounded_buffer_livelock high; } > $(LOGDIR)/bounded_buffer_livelock_high.log 2>&1
	{ $(BUILD)/bounded_buffer_livelock low;  } > $(LOGDIR)/bounded_buffer_livelock_low.log  2>&1
	{ $(BUILD)/backpressure_scenarios high; } > $(LOGDIR)/backpressure_scenarios_high.log 2>&1
	{ $(BUILD)/backpressure_scenarios low;  } > $(LOGDIR)/backpressure_scenarios_low.log  2>&1
	{ $(BUILD)/tests; } > $(LOGDIR)/tests.log 2>&1
	@echo "logs written to $(LOGDIR)/"

$(LOGDIR):
	mkdir -p $(LOGDIR)

clean:
	rm -rf $(BUILD) $(LOGDIR)
