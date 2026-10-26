# Host-side unit tests for the pure C++ parts of the components. No ESP32 needed.
#   make -C tests

CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g
BUILD := build
COMPONENTS := $(abspath ../components)
INCLUDE := $(BUILD)/include
DEFINES := -DFIXTURE_DIR='"$(abspath fixtures)"'

UPS_HID_SRC := $(COMPONENTS)/ups_hid/hid_parser.cpp $(COMPONENTS)/ups_hid/nut_mapping.cpp $(COMPONENTS)/ups_hid/quirks.cpp
NUT_SRC := $(COMPONENTS)/nut_server/nut_protocol.cpp
HEADERS := $(wildcard $(COMPONENTS)/*/*.h) $(wildcard *.h)

.PHONY: test clean
test: $(BUILD)/test_ups_hid $(BUILD)/test_nut_protocol
	./$(BUILD)/test_ups_hid
	./$(BUILD)/test_nut_protocol

# Mirror ESPHome's include layout: "esphome/components/<name>/<file>.h"
$(INCLUDE)/esphome/components:
	mkdir -p $(INCLUDE)/esphome
	ln -sfn $(COMPONENTS) $@

$(BUILD)/test_ups_hid: test_ups_hid.cpp $(UPS_HID_SRC) $(HEADERS) | $(INCLUDE)/esphome/components
	$(CXX) $(CXXFLAGS) $(DEFINES) -I$(INCLUDE) -o $@ test_ups_hid.cpp $(UPS_HID_SRC)

$(BUILD)/test_nut_protocol: test_nut_protocol.cpp $(NUT_SRC) $(HEADERS) | $(INCLUDE)/esphome/components
	$(CXX) $(CXXFLAGS) $(DEFINES) -I$(INCLUDE) -o $@ test_nut_protocol.cpp $(NUT_SRC)

clean:
	rm -rf $(BUILD)
