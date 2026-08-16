CXX ?= g++

CPPFLAGS := -I../../game
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS)
THINK_US ?= 240000
RELEASE_FLAGS := -O3 -DNDEBUG -fPIC -flto -fno-exceptions -fno-rtti \
	-fvisibility=hidden -fno-semantic-interposition
LINK_FLAGS := -shared -Wl,-z,defs -Wl,--as-needed \
	-Wl,--version-script=exports.map
TEST_DEFINES := -DSLOW_PLAYER_THINK_US=0 -DSLOW_PLAYER_EXHAUSTIVE=0
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined

TARGET := player.so
SOURCES := player.cpp strategy.cpp
HEADERS := strategy.h ../../game/game_api.h
TEST_BIN := test_runner
SANITIZER_BIN := test_runner_san
BENCH_BIN := bench_runner
FUZZ_CASES ?= 5000
SAN_FUZZ_CASES ?= 2000
BENCH_SAMPLES ?= 100
BENCH_RUNS ?= 2

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) exports.map Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
		-DSLOW_PLAYER_THINK_US=$(THINK_US) -DSLOW_PLAYER_EXHAUSTIVE=1 \
		$(LINK_FLAGS) -o $@ $(SOURCES)

verify: $(TARGET)
	file $(TARGET)
	test "$$(nm -D --defined-only $(TARGET) | \
		awk '$$3 == "moveDecision" { count++ } END { print count + 0 }')" -eq 1
	test "$$(stat -c %s $(TARGET))" -lt 16777216
	ldd $(TARGET)
	readelf -d $(TARGET)

$(TEST_BIN): tests.cpp $(SOURCES) $(HEADERS) Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O2 -g $(TEST_DEFINES) \
		-o $@ tests.cpp $(SOURCES)

$(SANITIZER_BIN): tests.cpp $(SOURCES) $(HEADERS) Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(SANITIZER_FLAGS) \
		$(TEST_DEFINES) -o $@ tests.cpp $(SOURCES)

test: $(TEST_BIN)
	./$(TEST_BIN) --fuzz $(FUZZ_CASES)

sanitize: $(SANITIZER_BIN)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(SANITIZER_BIN) --fuzz $(SAN_FUZZ_CASES)

$(BENCH_BIN): bench.cpp ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O3 -DNDEBUG -o $@ bench.cpp -ldl

bench-run: $(TARGET) $(BENCH_BIN)
	./$(BENCH_BIN) $(BENCH_SAMPLES) $(BENCH_RUNS) ./$(TARGET)

bench: bench-run

clean:
	rm -f $(TARGET) $(TEST_BIN) $(SANITIZER_BIN) $(BENCH_BIN)

.DELETE_ON_ERROR:
.PHONY: all verify test sanitize bench bench-run clean
