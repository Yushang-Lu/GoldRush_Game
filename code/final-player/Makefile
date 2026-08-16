CXX ?= g++

CPPFLAGS := -I../../game
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS)
RELEASE_FLAGS := -O3 -DNDEBUG -fPIC -flto -fno-exceptions -fno-rtti \
	-fvisibility=hidden -fno-semantic-interposition -fno-stack-protector \
	-march=x86-64-v3
LINK_FLAGS := -shared -Wl,-z,defs -Wl,--as-needed \
	-Wl,--version-script=exports.map
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined

TARGET := player.so
CORE_SOURCES := strategy_core.cpp legacy_fast.cpp
HEADERS := strategy_core.h ../../game/game_api.h \
	../fast-player2/fast-player2.cpp
CANDIDATES := candidates/fast_only.so candidates/always_deep.so \
	candidates/adaptive_no_hotspots.so \
	candidates/adaptive_no_block_inference.so
TEST_BIN := test_runner
SANITIZER_BIN := test_runner_san
SPARSE_BENCH_BIN := bench_sparse
DENSE_BENCH_BIN := bench_dense
SPARSE_SAMPLES ?= 100000
DENSE_SAMPLES ?= 50000
BENCH_RUNS ?= 3
NEW_LOGS ?= ../../new-logs/*.log
OLD_LOGS ?= ../../logs/*.log

all: $(TARGET)

$(TARGET): player.cpp $(CORE_SOURCES) $(HEADERS) exports.map Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(LINK_FLAGS) \
		-o $@ player.cpp $(CORE_SOURCES)

candidates/fast_only.so: candidates/fast_only.cpp $(CORE_SOURCES) $(HEADERS) exports.map Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(LINK_FLAGS) \
		-o $@ candidates/fast_only.cpp $(CORE_SOURCES)

candidates/always_deep.so: candidates/always_deep.cpp $(CORE_SOURCES) $(HEADERS) exports.map Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(LINK_FLAGS) \
		-o $@ candidates/always_deep.cpp $(CORE_SOURCES)

candidates/adaptive_no_hotspots.so: candidates/adaptive_no_hotspots.cpp $(CORE_SOURCES) $(HEADERS) exports.map Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(LINK_FLAGS) \
		-o $@ candidates/adaptive_no_hotspots.cpp $(CORE_SOURCES)

candidates/adaptive_no_block_inference.so: candidates/adaptive_no_block_inference.cpp $(CORE_SOURCES) $(HEADERS) exports.map Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(LINK_FLAGS) \
		-o $@ candidates/adaptive_no_block_inference.cpp $(CORE_SOURCES)

candidates: $(CANDIDATES)

verify: $(TARGET)
	file $(TARGET)
	test "$$(nm -D --defined-only $(TARGET) | \
		awk '$$3 == "moveDecision" { count++ } END { print count + 0 }')" -eq 1
	test "$$(stat -c %s $(TARGET))" -lt 16777216
	ldd $(TARGET)
	readelf -d $(TARGET)

$(TEST_BIN): tests.cpp player.cpp $(CORE_SOURCES) $(HEADERS) Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O2 -g -march=x86-64-v3 \
		-o $@ tests.cpp player.cpp $(CORE_SOURCES)

$(SANITIZER_BIN): tests.cpp player.cpp $(CORE_SOURCES) $(HEADERS) Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(SANITIZER_FLAGS) \
		-march=x86-64-v3 -o $@ tests.cpp player.cpp $(CORE_SOURCES)

test: $(TEST_BIN)
	./$(TEST_BIN) --fuzz 50000

sanitize: $(SANITIZER_BIN)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(SANITIZER_BIN) --fuzz 20000

$(SPARSE_BENCH_BIN): ../tests/bench.cpp ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O3 -DNDEBUG \
		-o $@ ../tests/bench.cpp -ldl

$(DENSE_BENCH_BIN): bench_dense.cpp ../../game/game_api.h Makefile
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O3 -DNDEBUG \
		-o $@ bench_dense.cpp -ldl

bench: $(TARGET) $(SPARSE_BENCH_BIN) $(DENSE_BENCH_BIN)
	./$(SPARSE_BENCH_BIN) $(SPARSE_SAMPLES) $(BENCH_RUNS)
	./$(DENSE_BENCH_BIN) $(DENSE_SAMPLES) $(BENCH_RUNS)

regression: $(TARGET)
	python3 -B tools/decision_diff.py ../fast-player2/player.so \
		$(TARGET) $(OLD_LOGS) --own-id 1

report: $(TARGET) candidates/adaptive_no_block_inference.so
	python3 -B tools/log_report.py $(NEW_LOGS) --own-id 2 \
		--strategy ../fast-player2/player.so \
		--strategy candidates/adaptive_no_block_inference.so \
		--strategy $(TARGET)

simulate: $(TARGET) $(CANDIDATES)
	python3 -B tools/synthetic_maze.py ../fast-player2/player.so \
		$(CANDIDATES) $(TARGET) --games 30

clean:
	rm -f $(TARGET) $(CANDIDATES) $(TEST_BIN) $(SANITIZER_BIN) \
		$(SPARSE_BENCH_BIN) $(DENSE_BENCH_BIN)
	rm -rf tools/__pycache__

.DELETE_ON_ERROR:
.PHONY: all candidates verify test sanitize bench regression report simulate clean
