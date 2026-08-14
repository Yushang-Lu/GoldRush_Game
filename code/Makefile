CXX ?= g++

CPPFLAGS := -I../game
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS)
RELEASE_FLAGS := -O3 -DNDEBUG -fPIC -flto -fno-exceptions -fno-rtti \
	-fvisibility=hidden -fno-semantic-interposition -fno-stack-protector \
	-march=x86-64-v3
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined

TARGET := player.so
TEST_BIN := tests/test_player
SANITIZER_BIN := tests/test_player_san
BENCH_BIN := tests/bench
BENCH_SAMPLES ?= 50000
BENCH_RUNS ?= 3
LOG_ANALYZER := tools/log_analyzer.py
REPLAY_COMPARE := tools/replay_compare.py
LOGS ?= ../logs/*.log
BASELINE_SO ?= tests/player_baseline.so
PERF_RATIO ?= 0.05
VERSUS_RATIO ?= 0.95
OPPONENT_SO ?= tests/info_player2.so
OPPONENT_SRC ?= ../info/player2.cpp

all: $(TARGET)

$(TARGET): player.cpp ../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
		-shared -Wl,-z,defs -Wl,--as-needed -o $@ player.cpp

$(TEST_BIN): tests/test_player.cpp player.cpp ../game/game_api.h
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O2 -g \
		-o $@ tests/test_player.cpp player.cpp

$(SANITIZER_BIN): tests/test_player.cpp player.cpp ../game/game_api.h
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(SANITIZER_FLAGS) \
		-o $@ tests/test_player.cpp player.cpp

$(BENCH_BIN): tests/bench.cpp ../game/game_api.h
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O3 -DNDEBUG \
		-o $@ tests/bench.cpp -ldl

$(OPPONENT_SO): $(OPPONENT_SRC) ../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
		-shared -Wl,-z,defs -Wl,--as-needed -o $@ $(OPPONENT_SRC)

test: $(TEST_BIN)
	./$(TEST_BIN) --fuzz 50000

sanitize: $(SANITIZER_BIN)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(SANITIZER_BIN) --fuzz 20000

bench: $(TARGET) $(BENCH_BIN)
	./$(BENCH_BIN) $(BENCH_SAMPLES) $(BENCH_RUNS)

versus: $(TARGET) $(BENCH_BIN) $(OPPONENT_SO)
	python3 -B $(REPLAY_COMPARE) $(OPPONENT_SO) $(TARGET) $(LOGS)
	./$(BENCH_BIN) 100000 5 $(OPPONENT_SO) $(VERSUS_RATIO)

logs:
	python3 -B $(LOG_ANALYZER) $(LOGS)

replay: $(TARGET)
	test -f $(BASELINE_SO)
	python3 -B $(REPLAY_COMPARE) $(BASELINE_SO) $(TARGET) $(LOGS)

compare: $(TARGET) $(BENCH_BIN)
	test -f $(BASELINE_SO)
	python3 -B $(REPLAY_COMPARE) $(BASELINE_SO) $(TARGET) $(LOGS)
	./$(BENCH_BIN) 100000 3 $(BASELINE_SO) $(PERF_RATIO)

clean:
	rm -f $(TARGET) $(TEST_BIN) $(SANITIZER_BIN) $(BENCH_BIN) \
		tests/info_player2.so
	rm -rf tools/__pycache__

.PHONY: all test sanitize bench versus logs replay compare clean
