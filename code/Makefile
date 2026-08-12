CXX ?= g++

CPPFLAGS := -I../game
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS)
RELEASE_FLAGS := -O3 -DNDEBUG -fPIC -flto -fno-exceptions -fno-rtti
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined

TARGET := player.so
TEST_BIN := tests/test_player
SANITIZER_BIN := tests/test_player_san
BENCH_BIN := tests/bench

all: $(TARGET)

$(TARGET): player.cpp ../game/game_api.h
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

test: $(TEST_BIN)
	./$(TEST_BIN) --fuzz 50000

sanitize: $(SANITIZER_BIN)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./$(SANITIZER_BIN) --fuzz 20000

bench: $(TARGET) $(BENCH_BIN)
	./$(BENCH_BIN) 50000 3

clean:
	rm -f $(TARGET) $(TEST_BIN) $(SANITIZER_BIN) $(BENCH_BIN)

.PHONY: all test sanitize bench clean
