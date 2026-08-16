#include "game_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using Decision = GameOutput (*)(const GameInput*);
using Clock = std::chrono::steady_clock;
constexpr int kRounds = 500;
constexpr const char* kMaze[GRID_SIZE] = {
    "..#...#...#...#..", ".##.#.##.##.#.##.",
    "....#.......#....", ".#######.#######.",
    ".......#.#.......", "######.#.#.######",
    ".....#.....#.....", ".###.###.###.###.",
    "...#.........#...", ".#.#.###.###.#.#.",
    ".#.#.#.....#.#.#.", ".#.#.#.#.#.#.#.#.",
    ".#...#.#.#.#...#.", ".###.#.###.#.###.",
    ".................", ".###.###.###.###.",
    "..#...#...#...#.."};
constexpr Position kOuter[] = {
    {0, 16}, {1, 16}, {2, 16}, {2, 15}, {2, 14}, {2, 13}, {1, 13},
    {0, 13}, {0, 12}, {0, 11}, {0, 9},  {0, 8},  {0, 7},  {0, 5},
    {0, 4},  {0, 3},  {1, 3},  {2, 3},  {2, 2},  {2, 1}};
constexpr Position kCenter[] = {
    {16, 0}, {15, 0}, {14, 0}, {14, 1}, {14, 2}, {14, 3}, {14, 4},
    {13, 4}, {12, 4}, {11, 4}, {10, 4}, {9, 4},  {8, 4},  {8, 5},
    {8, 6},  {8, 7},  {8, 8},  {6, 8},  {6, 9},  {6, 10}, {8, 10},
    {8, 11}, {8, 12}, {10, 12}, {10, 11}, {10, 10}, {10, 9}, {10, 8}};

bool inside(Position position) {
    return static_cast<unsigned>(position.row) <
               static_cast<unsigned>(GRID_SIZE) &&
           static_cast<unsigned>(position.col) <
               static_cast<unsigned>(GRID_SIZE);
}

std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void reveal(GameInput& input, Position center, int round, int salt) {
    for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
            const Position position{center.row + dr, center.col + dc};
            if (!inside(position)) continue;
            if (kMaze[position.row][position.col] == '#') {
                input.grid[position.row][position.col] = -1;
                continue;
            }
            const std::uint64_t value = mix(
                static_cast<std::uint64_t>(round * 977 + salt * 131 +
                                           position.row * 17 + position.col));
            int tile = 0;
            if (value % 61 == 0) tile = -3;
            else if (value % 4 == 0) {
                tile = 1 + static_cast<int>((value >> 11) % 120);
            }
            input.grid[position.row][position.col] = tile;
        }
    }
}

GameInput makeTurn(int round) {
    GameInput input{};
    input.round = round;
    for (auto& row : input.grid) {
        for (int& value : row) value = -5;
    }
    if (round == 0) {
        input.my_units[0] = Position{0, 16};
        input.my_units[1] = Position{16, 0};
    } else {
        input.my_units[0] =
            kOuter[(round / 2) % static_cast<int>(std::size(kOuter))];
        input.my_units[1] =
            kCenter[(round / 2) % static_cast<int>(std::size(kCenter))];
    }
    if (input.my_units[0].row == input.my_units[1].row &&
        input.my_units[0].col == input.my_units[1].col) {
        input.my_units[1] = Position{8, 8};
    }
    input.my_units_gold[0] = round * 3 + round % 17;
    input.my_units_gold[1] = round * 5 + round % 31;
    input.gold_opp = round * 9;
    reveal(input, input.my_units[0], round, 1);
    reveal(input, input.my_units[1], round, 2);
    input.grid[input.my_units[0].row][input.my_units[0].col] = 0;
    input.grid[input.my_units[1].row][input.my_units[1].col] = 0;
    input.visible_enemies[0] = Position{-1, -1};
    input.visible_enemies[1] = Position{-1, -1};
    if (round % 4 == 0) {
        Position enemy{input.my_units[1].row, input.my_units[1].col + 2};
        if (inside(enemy) && kMaze[enemy.row][enemy.col] != '#' &&
            !(enemy.row == input.my_units[0].row &&
              enemy.col == input.my_units[0].col)) {
            input.visible_enemies[0] = enemy;
            input.grid[enemy.row][enemy.col] = 0;
        }
    }
    input.num_visible_npcs = round % (MAX_NPCS + 1);
    for (int index = 0; index < MAX_NPCS; ++index) {
        input.visible_npcs[index].id = 0;
        input.visible_npcs[index].pos = Position{-1, -1};
    }
    for (int index = 0; index < input.num_visible_npcs; ++index) {
        input.visible_npcs[index].id = index + 1;
        input.visible_npcs[index].pos =
            Position{6 + (round + index) % 5,
                     6 + (round * 2 + index * 3) % 5};
    }
    input.snapshot_valid = round > 0 && round % 5 == 0;
    input.snapshot.window_begin = input.snapshot_valid ? round - 5 : -1;
    input.snapshot.window_end = input.snapshot_valid ? round - 1 : -1;
    for (int index = 0; index < REGION_COUNT; ++index) {
        input.snapshot.regions[index] = RegionStat{
            index + 1, (round + index) % 10, (round + index * 2) % 10,
            20 + (round * 3 + index * 17) % 120,
            10 + (round * 5 + index * 11) % 90,
            15 + (round * 7 + index * 23) % 180,
            (round + index) % 8};
    }
    return input;
}

GameInput makeClosedTurn(int round, const Position positions[2]) {
    GameInput input = makeTurn(round);
    for (auto& row : input.grid) {
        for (int& value : row) value = -5;
    }
    input.my_units[0] = positions[0];
    input.my_units[1] = positions[1];
    reveal(input, positions[0], round, 1);
    reveal(input, positions[1], round, 2);
    input.grid[positions[0].row][positions[0].col] = 0;
    input.grid[positions[1].row][positions[1].col] = 0;
    input.visible_enemies[0] = Position{-1, -1};
    input.visible_enemies[1] = Position{-1, -1};
    return input;
}

void applyOutput(const GameOutput& output, Position positions[2]) {
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == 4) continue;
            static constexpr int dr[4] = {-1, 1, 0, 0};
            static constexpr int dc[4] = {0, 0, -1, 1};
            const Position next{positions[unit].row + dr[action],
                                positions[unit].col + dc[action]};
            if (!inside(next) || kMaze[next.row][next.col] == '#' ||
                (next.row == positions[unit ^ 1].row &&
                 next.col == positions[unit ^ 1].col)) {
                continue;
            }
            positions[unit] = next;
        }
    }
}

Decision load(const char* path, void*& library) {
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::cerr << "dlopen failed: " << dlerror() << '\n';
        return nullptr;
    }
    void* symbol = dlsym(library, "moveDecision");
    const char* error = dlerror();
    if (error != nullptr) {
        std::cerr << "dlsym failed: " << error << '\n';
        return nullptr;
    }
    Decision decision = nullptr;
    static_assert(sizeof(decision) == sizeof(symbol));
    std::memcpy(&decision, &symbol, sizeof(decision));
    return decision;
}

double percentile(const std::vector<std::int64_t>& sorted, double fraction) {
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]) / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t samples = argc >= 2
                                    ? static_cast<std::size_t>(
                                          std::strtoull(argv[1], nullptr, 10))
                                    : 50000;
    const int runs = argc >= 3 ? std::atoi(argv[2]) : 3;
    const char* path = argc >= 4 ? argv[3] : "./player.so";
    if (samples == 0 || runs <= 0 || argc > 4) {
        std::cerr << "usage: " << argv[0]
                  << " [SAMPLES [RUNS [STRATEGY_SO]]]\n";
        return 2;
    }
    void* library = nullptr;
    const Decision decide = load(path, library);
    if (decide == nullptr) return 1;
    Position positions[2] = {{0, 16}, {16, 0}};
    GameInput turn = makeClosedTurn(0, positions);

    const auto firstBegin = Clock::now();
    const GameOutput first = decide(&turn);
    const auto firstEnd = Clock::now();
    std::uint64_t checksum = static_cast<std::uint64_t>(first.k + 1);
    std::cout << std::fixed << std::setprecision(3)
              << "first_call_us="
              << std::chrono::duration<double, std::micro>(firstEnd - firstBegin)
                     .count()
              << '\n';
    for (int game = 0; game < 4; ++game) {
        positions[0] = Position{0, 16};
        positions[1] = Position{16, 0};
        for (int round = 0; round < kRounds; ++round) {
            turn = makeClosedTurn(round, positions);
            const GameOutput output = decide(&turn);
            applyOutput(output, positions);
            checksum = checksum * 1315423911ULL +
                       static_cast<std::uint64_t>(output.actions[game % S] +
                                                  output.order * 7 + 1);
        }
    }
    std::vector<std::int64_t> values(samples);
    for (int run = 1; run <= runs; ++run) {
        for (std::size_t sample = 0; sample < samples; ++sample) {
            const int round = static_cast<int>(sample % kRounds);
            if (round == 0) {
                positions[0] = Position{0, 16};
                positions[1] = Position{16, 0};
            }
            turn = makeClosedTurn(round, positions);
            const auto begin = Clock::now();
            const GameOutput output = decide(&turn);
            const auto end = Clock::now();
            applyOutput(output, positions);
            values[sample] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                    .count();
            checksum = checksum * 11400714819323198485ULL +
                       static_cast<std::uint64_t>(output.actions[sample % S] +
                                                  output.k * 7 +
                                                  output.order * 17 + 1);
        }
        std::sort(values.begin(), values.end());
        std::cout << "run=" << run << " samples=" << samples
                  << " p50_us=" << percentile(values, 0.50)
                  << " p90_us=" << percentile(values, 0.90)
                  << " p99_us=" << percentile(values, 0.99)
                  << " max_us="
                  << static_cast<double>(values.back()) / 1000.0
                  << " checksum=" << checksum << '\n';
    }
    dlclose(library);
    return checksum == 0 ? 3 : 0;
}
