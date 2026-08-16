#include "game_api.h"

#include <algorithm>
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
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};
constexpr const char* kMaze[GRID_SIZE] = {
    "..#...#...#...#..", ".##.#.##.##.#.##.", "....#.......#....",
    ".#######.#######.", ".......#.#.......", "######.#.#.######",
    ".....#.....#.....", ".###.###.###.###.", "...#.........#...",
    ".#.#.###.###.#.#.", ".#.#.#.....#.#.#.", ".#.#.#.#.#.#.#.#.",
    ".#...#.#.#.#...#.", ".###.#.###.#.###.", ".................",
    ".###.###.###.###.", "..#...#...#...#.."};

bool inside(Position position) {
    return static_cast<unsigned>(position.row) <
               static_cast<unsigned>(GRID_SIZE) &&
           static_cast<unsigned>(position.col) <
               static_cast<unsigned>(GRID_SIZE);
}

bool same(Position first, Position second) {
    return first.row == second.row && first.col == second.col;
}

std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
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
            const std::uint64_t random = mix(
                static_cast<std::uint64_t>(round * 977 + salt * 131 +
                                           position.row * 17 + position.col));
            int value = 0;
            if (random % 89U == 0U) {
                value = -3;
            } else if (random % 5U == 0U) {
                value = 1 + static_cast<int>((random >> 11U) % 140U);
            }
            input.grid[position.row][position.col] = value;
        }
    }
}

GameInput makeInput(int round, const Position positions[2]) {
    GameInput input{};
    input.round = round;
    for (auto& row : input.grid) {
        for (int& value : row) value = -5;
    }
    input.my_units[0] = positions[0];
    input.my_units[1] = positions[1];
    input.my_units_gold[0] = round * 3 + round % 17;
    input.my_units_gold[1] = round * 5 + round % 29;
    input.gold_opp = round * 9;
    reveal(input, positions[0], round, 1);
    reveal(input, positions[1], round, 2);
    input.grid[positions[0].row][positions[0].col] = 0;
    input.grid[positions[1].row][positions[1].col] = 0;
    input.visible_enemies[0] = Position{-1, -1};
    input.visible_enemies[1] = Position{-1, -1};
    if (round % 4 == 0) {
        const Position enemy{8, 8};
        if (!same(enemy, positions[0]) && !same(enemy, positions[1])) {
            input.visible_enemies[0] = enemy;
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
            Position{7 + index % 3, 7 + (round + index) % 3};
    }
    input.snapshot_valid = round > 0 && round % 5 == 0;
    input.snapshot.window_begin = input.snapshot_valid ? round - 5 : -1;
    input.snapshot.window_end = input.snapshot_valid ? round - 1 : -1;
    for (int index = 0; index < REGION_COUNT; ++index) {
        input.snapshot.regions[index] = RegionStat{
            index + 1,
            (round + index) % 9,
            (round + index * 2) % 8,
            index == 0 ? 35 + round % 31
                       : (round % 15 == 5 && index == 1 + round / 15 % 4
                              ? 100 + round % 31
                              : round % 13),
            10 + (round + index * 5) % 30,
            15 + (round * 3 + index * 17) % 130,
            (round + index) % 8};
    }
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
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            if (!inside(next) || kMaze[next.row][next.col] == '#' ||
                same(next, positions[unit ^ 1])) {
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
    dlerror();
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

bool validOutput(const GameOutput& output) {
    for (int action : output.actions) {
        if (action < 0 || action > 4) return false;
    }
    return output.k >= 0 && output.k <= S &&
           (output.order == 0 || output.order == 1) &&
           output.vp >= 0 && output.vp <= 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t samples =
        argc >= 2 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
                  : 100U;
    const int runs = argc >= 3 ? std::atoi(argv[2]) : 2;
    const char* path = argc >= 4 ? argv[3] : "./player.so";
    if (samples == 0U || runs <= 0 || argc > 4) {
        std::cerr << "usage: " << argv[0]
                  << " [SAMPLES [RUNS [STRATEGY_SO]]]\n";
        return 2;
    }
    void* library = nullptr;
    const Decision decision = load(path, library);
    if (decision == nullptr) return 1;

    Position positions[2] = {{0, 16}, {16, 0}};
    GameInput input = makeInput(0, positions);
    const auto firstBegin = Clock::now();
    const GameOutput first = decision(&input);
    const auto firstEnd = Clock::now();
    if (!validOutput(first)) {
        std::cerr << "strategy returned an invalid first output\n";
        dlclose(library);
        return 1;
    }
    std::uint64_t checksum = static_cast<std::uint64_t>(first.k + 1);
    std::cout << std::fixed << std::setprecision(3)
              << "first_call_us="
              << std::chrono::duration<double, std::micro>(firstEnd - firstBegin)
                     .count()
              << '\n';

    std::vector<std::int64_t> values(samples);
    for (int run = 1; run <= runs; ++run) {
        positions[0] = Position{0, 16};
        positions[1] = Position{16, 0};
        for (std::size_t sample = 0; sample < samples; ++sample) {
            const int round = static_cast<int>(sample % 500U);
            if (round == 0) {
                positions[0] = Position{0, 16};
                positions[1] = Position{16, 0};
            }
            input = makeInput(round, positions);
            const auto begin = Clock::now();
            const GameOutput output = decision(&input);
            const auto end = Clock::now();
            if (!validOutput(output)) {
                std::cerr << "strategy returned an invalid output at sample "
                          << sample << '\n';
                dlclose(library);
                return 1;
            }
            applyOutput(output, positions);
            values[sample] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                    .count();
            checksum = checksum * 11400714819323198485ULL +
                       static_cast<std::uint64_t>(
                           output.actions[sample % S] + output.k * 7 +
                           output.order * 17 + output.vp * 31 + 1);
        }
        std::sort(values.begin(), values.end());
        std::cout << "run=" << run << " samples=" << samples
                  << " p50_us=" << percentile(values, 0.50)
                  << " p90_us=" << percentile(values, 0.90)
                  << " p99_us=" << percentile(values, 0.99)
                  << " max_us=" << static_cast<double>(values.back()) / 1000.0
                  << '\n';
    }
    std::cout << "checksum=" << checksum << '\n';
    dlclose(library);
    return 0;
}
