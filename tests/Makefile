# Unit tests for the FPP-free effects hold in src/core. Any C++17 compiler on macOS
# or Linux will do; FPP is not needed.
#
#   make -C tests          build and run
#   make -C tests clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CORE := ../src/core

SOURCES := EffectsHoldTests.cpp
HEADERS := $(CORE)/EffectsHold.h

all: run

effects_hold_tests: $(SOURCES) $(HEADERS) Makefile
	$(CXX) $(CXXFLAGS) -I$(CORE) $(SOURCES) -o $@

run: effects_hold_tests
	./effects_hold_tests

clean:
	rm -rf effects_hold_tests effects_hold_tests.dSYM

.PHONY: all run clean
