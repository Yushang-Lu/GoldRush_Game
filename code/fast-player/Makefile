CXX ?= g++

CPPFLAGS := -I../../game
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS)
RELEASE_FLAGS := -O3 -DNDEBUG -fPIC -flto -fno-exceptions -fno-rtti \
	-fvisibility=hidden -fno-semantic-interposition -fno-stack-protector \
	-march=x86-64-v3

TARGET := player.so
SOURCE := fast-player.cpp
BENCH_BIN := bench_runner
BENCH_SOURCE := ../tests/bench.cpp
BASELINE_SO := baseline-player.so
BASELINE_SOURCE := ../player.cpp

BENCH_SAMPLES ?= 50000
BENCH_RUNS ?= 3
COMPARE_SAMPLES ?= 100000
COMPARE_RUNS ?= 3
MAX_RATIO ?= 0.50

all: $(TARGET)

$(TARGET): $(SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
		-shared -Wl,-z,defs -Wl,--as-needed -o $@ $(SOURCE)

$(BENCH_BIN): $(BENCH_SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O3 -DNDEBUG \
		-o $@ $(BENCH_SOURCE) -ldl

$(BASELINE_SO): $(BASELINE_SOURCE) ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
		-shared -Wl,-z,defs -Wl,--as-needed -o $@ $(BASELINE_SOURCE)

verify: $(TARGET)
	file $(TARGET)
	test "$$(nm -D --defined-only $(TARGET) | \
		awk '$$3 == "moveDecision" { count++ } END { print count + 0 }')" -eq 1
	test "$$(stat -c %s $(TARGET))" -lt 16777216
	ldd $(TARGET)

bench: $(TARGET) $(BENCH_BIN)
	./$(BENCH_BIN) $(BENCH_SAMPLES) $(BENCH_RUNS)

compare: $(TARGET) $(BENCH_BIN) $(BASELINE_SO)
	./$(BENCH_BIN) $(COMPARE_SAMPLES) $(COMPARE_RUNS) \
		./$(BASELINE_SO) $(MAX_RATIO)

clean:
	rm -f $(TARGET) $(BENCH_BIN) $(BASELINE_SO)

.DELETE_ON_ERROR:
.PHONY: all verify bench compare clean
