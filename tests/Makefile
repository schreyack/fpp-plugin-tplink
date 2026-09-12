# Unit tests for the FPP-free code in src/core, run against a fake Kasa plug on
# 127.0.0.1. Any C++17 compiler on macOS or Linux will do; FPP is not needed.
#
#   make -C tests          build and run
#   make -C tests clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CORE := ../src/core

SOURCES := tplink_tests.cpp SequenceTests.cpp GiveUpTests.cpp ProtocolTests.cpp FakeKasaPlug.cpp \
           $(CORE)/KasaProtocol.cpp $(CORE)/KasaSocket.cpp $(CORE)/MiniJson.cpp $(CORE)/RelaySender.cpp
HEADERS := TestHarness.h FakeKasaPlug.h $(CORE)/KasaProtocol.h $(CORE)/MiniJson.h \
           $(CORE)/RelayFollower.h $(CORE)/RelaySender.h $(CORE)/SequenceRelay.h

all: run

tplink_tests: $(SOURCES) $(HEADERS) Makefile
	$(CXX) $(CXXFLAGS) -pthread -I$(CORE) -DFIXTURE_DIR='"$(CURDIR)/fixtures"' $(SOURCES) -o $@

run: tplink_tests
	./tplink_tests

clean:
	rm -rf tplink_tests tplink_tests.dSYM

.PHONY: all run clean
