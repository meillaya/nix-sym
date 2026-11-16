CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc $(shell pkg-config --cflags libdw libelf libzstd libarchive)
LDLIBS += $(shell pkg-config --libs libdw libelf libzstd libarchive)

SRC := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))
MAIN_OBJ := build/main.o
BIN := build/nix-sym
UNIT_TEST := build/unit_tests
RUST_TEST := build/rust_demangle_test
HASKELL_TEST := build/haskell_demangle_test
TESTS := $(UNIT_TEST) $(RUST_TEST) $(HASKELL_TEST)

.PHONY: all check test install clean format

all: $(BIN) $(TESTS)

build:
	mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/main.o: src/main.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BIN): $(OBJ) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(UNIT_TEST): tests/unit_tests.cpp $(OBJ) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(RUST_TEST): tests/unit/rust_demangle_test.cpp src/rust_demangle.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(HASKELL_TEST): tests/unit/haskell_demangle_test.cpp src/haskell_demangle.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

check: all
	./$(UNIT_TEST)
	./$(RUST_TEST)
	./$(HASKELL_TEST)

test: check

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/nix-sym

format:
	clang-format -i src/*.cpp src/*.hpp tests/*.cpp tests/unit/*.cpp

clean:
	rm -rf build
