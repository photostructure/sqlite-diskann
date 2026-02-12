# sqlite-diskann Makefile
# Cross-platform SQLite extension for DiskANN vector search

.PHONY: all clean test test-native test-all test-stress check asan valgrind bear lint clang-tidy fmt help

# Compiler and flags
CC ?= gcc
CFLAGS = -std=c17 -Wall -Wextra -Werror -pedantic -fPIC -O2
CFLAGS += -Wconversion -Wshadow -Wstrict-prototypes
CFLAGS += -Ivendor/sqlite  # Use vendored SQLite 3.51.2 headers
LDFLAGS = -shared
LIBS = -lm
# Note: Do NOT link -lsqlite3. SQLite symbols resolved from host at runtime.

# Extra flags for sanitizer builds (passed via recursive make)
EXTRA_CFLAGS ?=

# Directories
SRC_DIR = src
TEST_DIR = tests
BUILD_DIR = build

# Output
EXTENSION = diskann.so
TEST_BIN = test_diskann
STRESS_BIN = test_stress

# Source files
SOURCES = $(SRC_DIR)/diskann_api.c $(SRC_DIR)/diskann_blob.c $(SRC_DIR)/diskann_cache.c $(SRC_DIR)/diskann_insert.c $(SRC_DIR)/diskann_node.c $(SRC_DIR)/diskann_search.c $(SRC_DIR)/diskann_vtab.c
TEST_C_SOURCES = $(filter-out %/test_runner.c %/test_stress.c, $(wildcard $(TEST_DIR)/c/test_*.c))
TEST_RUNNER = $(TEST_DIR)/c/test_runner.c
UNITY_SOURCES = $(TEST_DIR)/c/unity/unity.c
SQLITE_SOURCE = vendor/sqlite/sqlite3.c

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    EXTENSION = diskann.dylib
    # Allow undefined symbols (resolved at runtime when loaded into SQLite)
    LDFLAGS += -dynamiclib -undefined dynamic_lookup
endif
ifeq ($(UNAME_S),Linux)
    EXTENSION = diskann.so
    # Allow undefined symbols (resolved at runtime when loaded into SQLite)
    # Equivalent to macOS's -undefined dynamic_lookup
    LDFLAGS += -Wl,--allow-shlib-undefined
endif

# Default target
all: $(BUILD_DIR)/$(EXTENSION)

# Check if test binary is instrumented (ASan/UBSan)
# Returns 0 if instrumented, 1 if clean
.PHONY: check-instrumented
check-instrumented:
	@if [ -f "$(BUILD_DIR)/$(TEST_BIN)" ]; then \
		if nm "$(BUILD_DIR)/$(TEST_BIN)" 2>/dev/null | grep -q '__asan\|__ubsan\|__tsan'; then \
			exit 0; \
		else \
			exit 1; \
		fi; \
	else \
		exit 1; \
	fi

# Release build (ensures clean, non-instrumented build)
# Only cleans if instrumented build detected - preserves incremental builds
release:
	@if $(MAKE) -s check-instrumented 2>/dev/null; then \
		echo "Instrumented build detected - cleaning..."; \
		$(MAKE) clean; \
	fi
	@$(MAKE) EXTRA_CFLAGS="" all

# Build extension (without sqlite3.o - symbols resolved from host at runtime)
$(BUILD_DIR)/$(EXTENSION): $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DDISKANN_EXTENSION $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Built: $@"

# Build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile SQLite separately (relaxed warnings - SQLite doesn't compile with -Wconversion)
# EXTRA_CFLAGS used for sanitizer instrumentation (e.g., ASan)
$(BUILD_DIR)/sqlite3.o: $(SQLITE_SOURCE) | $(BUILD_DIR)
	$(CC) -std=c17 -fPIC -O2 $(EXTRA_CFLAGS) -DSQLITE_OMIT_LOAD_EXTENSION -c -o $@ $<
	@echo "Built vendored SQLite 3.51.2: $@"

# Build tests
test: test-native

test-native: $(BUILD_DIR)/$(TEST_BIN)
	@echo "Running native C tests..."
	$(BUILD_DIR)/$(TEST_BIN)

test-all: test-native
	@echo "Running TypeScript tests..."
	@command -v npm >/dev/null 2>&1 || { echo "Error: npm not installed" >&2; exit 1; }
	npm run test:ts

# Build and run stress tests (separate from regular tests due to long runtime)
test-stress: $(BUILD_DIR)/$(STRESS_BIN)
	@echo "Running stress tests (this may take several minutes)..."
	$(BUILD_DIR)/$(STRESS_BIN)
	@echo "Cleaning up stress test database files..."
	@rm -f /tmp/diskann_stress_*.db*

$(BUILD_DIR)/$(STRESS_BIN): $(SOURCES) $(TEST_DIR)/c/test_stress.c $(UNITY_SOURCES) $(BUILD_DIR)/sqlite3.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -I$(SRC_DIR) -I$(TEST_DIR)/c -o $@ $^ $(LIBS)
	@echo "Built stress test suite: $@"

$(BUILD_DIR)/$(TEST_BIN): $(SOURCES) $(TEST_C_SOURCES) $(TEST_RUNNER) $(UNITY_SOURCES) $(BUILD_DIR)/sqlite3.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -DTESTING -I$(SRC_DIR) -I$(TEST_DIR)/c -o $@ $^ $(LIBS)
	@echo "Built test suite: $@"

# Run tests
check: test

# AddressSanitizer build (memory leak detection)
# Uses recursive make to avoid stale directory state after clean
ASAN_EXTRA = -fsanitize=address -fsanitize=undefined -g -O1 -fno-omit-frame-pointer
asan:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="$(ASAN_EXTRA)" $(BUILD_DIR)/$(TEST_BIN)
	@echo "Running with AddressSanitizer..."
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(BUILD_DIR)/$(TEST_BIN)

# Generate compile_commands.json for clang-tidy
bear:
	@command -v bear >/dev/null 2>&1 || { echo "Error: bear not installed. Install with: sudo apt install bear" >&2; exit 1; }
	bear -- $(MAKE) clean all $(BUILD_DIR)/$(TEST_BIN) || true
	@# Strip .o linker inputs that Bear captures (clang-tidy only compiles)
	@python3 -c "import json;cc=json.load(open('compile_commands.json'));[e.__setitem__('arguments',[a for a in e['arguments'] if not a.endswith('.o')]) for e in cc];json.dump(cc,open('compile_commands.json','w'),indent=2)"

# Run clang-tidy (requires compile_commands.json from bear)
clang-tidy:
	@command -v clang-tidy >/dev/null 2>&1 || { echo "Error: clang-tidy not installed" >&2; exit 1; }
	@if [ ! -f compile_commands.json ]; then \
		echo "Error: compile_commands.json not found. Run 'make bear' first." >&2; \
		exit 1; \
	fi
	clang-tidy $(SOURCES) $(TEST_C_SOURCES)

# Alias lint to clang-tidy for consistency with package.json
lint: clang-tidy

# Format C code with clang-format
fmt:
	@command -v clang-format >/dev/null 2>&1 || { echo "Error: clang-format not installed. Install with: sudo apt install clang-format" >&2; exit 1; }
	clang-format -i -style=LLVM $(SOURCES) $(SRC_DIR)/*.h $(TEST_C_SOURCES) $(TEST_RUNNER)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -f compile_commands.json

# Valgrind (memory leak detection - comprehensive but slower)
valgrind: $(BUILD_DIR)/$(TEST_BIN)
	@command -v valgrind >/dev/null 2>&1 || { echo "Error: valgrind not installed. Install with: sudo apt install valgrind" >&2; exit 1; }
	@echo "Running with Valgrind (this may take a while)..."
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 $(BUILD_DIR)/$(TEST_BIN)

# Help
help:
	@echo "sqlite-diskann build targets:"
	@echo "  all          Build extension (default)"
	@echo "  release      Ensure clean build (auto-detects and cleans ASan/UBSan builds)"
	@echo "  test         Build and run native C tests (alias for test-native)"
	@echo "  test-native  Build and run native C tests only"
	@echo "  test-all     Build and run both native C and TypeScript tests"
	@echo "  test-stress  Build and run stress tests (300k/100k vectors, takes ~5-10min)"
	@echo "  check        Alias for test"
	@echo "  asan         Build and run with AddressSanitizer (fast memory checks)"
	@echo "  valgrind     Build and run with Valgrind (thorough memory checks)"
	@echo "  bear         Generate compile_commands.json for clang-tidy"
	@echo "  clang-tidy   Run static analysis (auto-generates compile_commands.json if needed)"
	@echo "  clean        Remove build artifacts"
	@echo "  help         Show this help"
