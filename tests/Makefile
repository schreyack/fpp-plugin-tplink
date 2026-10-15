# Unit tests for the FPP-free code in src/core (the effects hold and device names).
# Any C++17 compiler on macOS or Linux will do; FPP is not needed.
#
#   make -C tests          build and run
#   make -C tests clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CORE := ../src/core

all: run

effects_hold_tests: EffectsHoldTests.cpp $(CORE)/EffectsHold.h Makefile
	$(CXX) $(CXXFLAGS) -I$(CORE) EffectsHoldTests.cpp -o $@

device_names_tests: DeviceNamesTests.cpp $(CORE)/DeviceNames.h Makefile
	$(CXX) $(CXXFLAGS) -I$(CORE) DeviceNamesTests.cpp -o $@

run: effects_hold_tests device_names_tests
	./effects_hold_tests
	./device_names_tests

clean:
	rm -rf effects_hold_tests effects_hold_tests.dSYM device_names_tests device_names_tests.dSYM

.PHONY: all run clean
