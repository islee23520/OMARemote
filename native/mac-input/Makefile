CXX = clang++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
PREFIX ?= $(HOME)/.local
DESTDIR ?=

.PHONY: all install check clean
all: omaremote-mac-input

# Run on macOS with Xcode Command Line Tools. Not part of the Linux build.
omaremote-mac-input: main.mm protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 main.mm -framework ApplicationServices -framework CoreGraphics -o $@

# Portable parser/map tests; no macOS frameworks and no event injection.
protocol-test: protocol-test.cpp protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 protocol-test.cpp -o $@

check: protocol-test
	./protocol-test

# User-owned installation only; never sudo, permissions, or service registration.
install: omaremote-mac-input
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 omaremote-mac-input "$(DESTDIR)$(PREFIX)/bin/omaremote-mac-input"

clean:
	rm -f omaremote-mac-input protocol-test
