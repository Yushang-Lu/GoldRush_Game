CXX ?= g++

CPPFLAGS := -I../../game
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS)
RELEASE_FLAGS := -O3 -DNDEBUG -fPIC -flto -fno-exceptions -fno-rtti \
	-fvisibility=hidden -fno-semantic-interposition -fno-stack-protector \
	-march=x86-64-v3
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined

TARGET := player.so
SOURCE := fast-player4.cpp
TEST_BIN := test_runner
TEST_SOURCE := fast-player4-tests.cpp
SANITIZER_BIN := test_runner_san
BENCH_BIN := bench_runner
BENCH_SOURCE := ../tests/bench.cpp
BASELINE_SO ?= ../fast-player2/player.so
LOGS ?= ../../logs/*.log
REPLAY_COMPARE := ../tools/replay_compare.py

BENCH_SAMPLES ?= 100000
BENCH_RUNS ?= 3
COMPARE_SAMPLES ?= 200000
COMPARE_RUNS ?= 5
MAX_RATIO ?= 3.50

all: $(TARGET)

$(TARGET): $(SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
		-shared -Wl,-z,defs -Wl,--as-needed -o $@ $(SOURCE)

$(TEST_BIN): $(TEST_SOURCE) $(SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O2 -g -march=x86-64-v3 \
		-o $@ $(TEST_SOURCE) $(SOURCE)

$(SANITIZER_BIN): $(TEST_SOURCE) $(SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(SANITIZER_FLAGS) \
		-march=x86-64-v3 -o $@ $(TEST_SOURCE) $(SOURCE)

$(BENCH_BIN): $(BENCH_SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O3 -DNDEBUG \
		-o $@ $(BENCH_SOURCE) -ldl

verify: $(TARGET)
	file $(TARGET)
	test "$$(nm -D --defined-only $(TARGET) | \
		awk '$$3 == "moveDecision" { count++ } END { print count + 0 }')" -eq 1
	test "$$(stat -c %s $(TARGET))" -lt 16777216
	ldd $(TARGET)
	readelf -d $(TARGET)

test: $(TEST_BIN)
	./$(TEST_BIN) --fuzz 50000

sanitize: $(SANITIZER_BIN)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(SANITIZER_BIN) --fuzz 20000

bench: $(TARGET) $(BENCH_BIN)
	./$(BENCH_BIN) $(BENCH_SAMPLES) $(BENCH_RUNS)

compare: $(TARGET) $(BENCH_BIN)
	test -f $(BASELINE_SO)
	./$(BENCH_BIN) $(COMPARE_SAMPLES) $(COMPARE_RUNS) \
		$(BASELINE_SO) $(MAX_RATIO)

replay: $(TARGET)
	test -f $(BASELINE_SO)
	python3 -B $(REPLAY_COMPARE) $(BASELINE_SO) $(TARGET) $(LOGS)

clean:
	rm -f $(TARGET) $(TEST_BIN) $(SANITIZER_BIN) $(BENCH_BIN)
	rm -rf ../tools/__pycache__

.DELETE_ON_ERROR:
.PHONY: all verify test sanitize bench compare replay clean
