CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc $(shell pkg-config --cflags libdw libelf libzstd libarchive libdebuginfod)
DEPFLAGS = -MMD -MP
LDLIBS += $(shell pkg-config --libs libdw libelf libzstd libarchive libdebuginfod) -pthread

SRC := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))
MAIN_OBJ := build/main.o
BIN := build/nix-sym
UNIT_TEST := build/unit_tests
RUST_TEST := build/rust_demangle_test
HASKELL_TEST := build/haskell_demangle_test
DEBUGINFOD_TEST := build/debuginfod_test
STACK_TEST := build/stack_test
CORE_READER_TEST := build/core_reader_test
ARCHIVE_TEST := build/archive_test
SOURCE_AUTO_TEST := build/source_auto_test
TESTS := $(UNIT_TEST) $(RUST_TEST) $(HASKELL_TEST) $(DEBUGINFOD_TEST) $(STACK_TEST) \
         $(CORE_READER_TEST) $(ARCHIVE_TEST) $(SOURCE_AUTO_TEST)

.PHONY: all check test install clean format sanitize fuzz

all: $(BIN) $(TESTS)

build:
	mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

build/main.o: src/main.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BIN): $(OBJ) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(UNIT_TEST): tests/unit_tests.cpp $(OBJ) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(RUST_TEST): tests/unit/rust_demangle_test.cpp src/rust_demangle.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(HASKELL_TEST): tests/unit/haskell_demangle_test.cpp src/haskell_demangle.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(DEBUGINFOD_TEST): tests/unit/debuginfod_test.cpp src/debuginfod.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ldebuginfod -o $@

$(STACK_TEST): tests/unit/stack_test.cpp src/stack.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(CORE_READER_TEST): tests/unit/core_reader_test.cpp $(OBJ) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(ARCHIVE_TEST): tests/unit/archive_test.cpp $(OBJ) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(SOURCE_AUTO_TEST): tests/unit/source_auto_test.cpp $(OBJ) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@

-include $(OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

check: all
	./$(UNIT_TEST)
	./$(RUST_TEST)
	./$(HASKELL_TEST)
	./$(DEBUGINFOD_TEST)
	./$(STACK_TEST)
	./$(CORE_READER_TEST)
	./$(ARCHIVE_TEST)
	./$(SOURCE_AUTO_TEST)

test: check

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/nix-sym

format:
	clang-format -i src/*.cpp src/*.hpp tests/*.cpp tests/unit/*.cpp fuzz/*.cpp

sanitize:
	bash scripts/check-sanitize.sh

fuzz:
	bash scripts/check-fuzz.sh

clean:
	rm -rf build
