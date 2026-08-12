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
#include <string>
#include <vector>

namespace {

using DecisionFunction = GameOutput (*)(const GameInput*);
using Clock = std::chrono::steady_clock;

constexpr int kTurns = 500;

std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool inside(Position pos) {
    return pos.row >= 0 && pos.row < GRID_SIZE && pos.col >= 0 &&
           pos.col < GRID_SIZE;
}

void reveal(GameInput& input, Position center, int radius,
            std::uint64_t seed) {
    for (int dr = -radius; dr <= radius; ++dr) {
        for (int dc = -radius; dc <= radius; ++dc) {
            const Position pos{center.row + dr, center.col + dc};
            if (!inside(pos)) {
                continue;
            }
            const std::uint64_t value =
                mix(seed + static_cast<std::uint64_t>(pos.row * GRID_SIZE +
                                                      pos.col) *
                               0x9e3779b97f4a7c15ULL);
            int tile = 0;
            if (value % 29 == 0) {
                tile = -1;
            } else if (value % 47 == 0) {
                tile = -3;
            } else if (value % 5 == 0) {
                tile = 1 + static_cast<int>((value >> 12) % 90);
            }
            input.grid[pos.row][pos.col] = tile;
        }
    }
}

GameInput makeTurn(int round) {
    GameInput input{};
    input.round = round;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            input.grid[row][col] = -5;
        }
    }

    const int phase = round % 56;
    input.my_units[0] = Position{1 + phase % 14,
                                 1 + (phase * 3 + round / 17) % 14};
    input.my_units[1] = Position{15 - phase % 14,
                                 15 - (phase * 5 + round / 23) % 14};
    if (input.my_units[0].row == input.my_units[1].row &&
        input.my_units[0].col == input.my_units[1].col) {
        input.my_units[1].col = (input.my_units[1].col + 2) % GRID_SIZE;
    }
    input.my_units_gold[0] = round * 7 + round % 31;
    input.my_units_gold[1] = round * 6 + round % 23;
    input.gold_opp = round * 12 + round % 101;

    const std::uint64_t seed =
        mix(0x123456789abcdef0ULL + static_cast<std::uint64_t>(round));
    reveal(input, input.my_units[0], 2, seed);
    reveal(input, input.my_units[1], 2, seed ^ 0xfeedfacecafebeefULL);
    input.grid[input.my_units[0].row][input.my_units[0].col] = 0;
    input.grid[input.my_units[1].row][input.my_units[1].col] = 0;

    for (int enemy = 0; enemy < 2; ++enemy) {
        input.visible_enemies[enemy] = Position{-1, -1};
    }
    if (round % 3 == 0) {
        Position enemy{input.my_units[0].row,
                       input.my_units[0].col + (round % 2 == 0 ? 2 : -2)};
        if (inside(enemy) &&
            (enemy.row != input.my_units[1].row ||
             enemy.col != input.my_units[1].col)) {
            input.visible_enemies[0] = enemy;
            input.grid[enemy.row][enemy.col] = 0;
        }
    }

    input.num_visible_npcs = round % (MAX_NPCS + 1);
    for (int npc = 0; npc < MAX_NPCS; ++npc) {
        input.visible_npcs[npc].id = 0;
        input.visible_npcs[npc].pos = Position{-1, -1};
    }
    for (int npc = 0; npc < input.num_visible_npcs; ++npc) {
        input.visible_npcs[npc].id = npc + 1;
        input.visible_npcs[npc].pos =
            Position{4 + (round + npc * 3) % 9,
                     4 + (round * 2 + npc * 5) % 9};
    }

    input.snapshot_valid = round % 5 == 0 ? 1 : 0;
    input.snapshot.window_begin = input.snapshot_valid ? std::max(0, round - 5)
                                                       : -1;
    input.snapshot.window_end = input.snapshot_valid ? round : -1;
    for (int region = 0; region < REGION_COUNT; ++region) {
        RegionStat& stat = input.snapshot.regions[region];
        stat.id = region + 1;
        stat.enter = (round + region * 2) % 15;
        stat.leave = (round * 2 + region) % 15;
        stat.gold_generated = 40 + (round * 7 + region * 13) % 120;
        stat.gold_collected = 20 + (round * 5 + region * 11) % 90;
        stat.gold_remaining = 10 + (round * 3 + region * 17) % 100;
        stat.occupants = (round + region) % 8;
    }
    return input;
}

double percentile(const std::vector<std::int64_t>& sorted, double fraction) {
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]) / 1000.0;
}

void printRun(int run, std::vector<std::int64_t>& samples,
              std::uint64_t checksum) {
    std::sort(samples.begin(), samples.end());
    std::cout << "run=" << run << " samples=" << samples.size()
              << std::fixed << std::setprecision(3)
              << " p50_us=" << percentile(samples, 0.50)
              << " p90_us=" << percentile(samples, 0.90)
              << " p99_us=" << percentile(samples, 0.99)
              << " max_us=" << static_cast<double>(samples.back()) / 1000.0
              << " checksum=" << checksum << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t samples_per_run = 50000;
    int runs = 3;
    if (argc >= 2) {
        samples_per_run = static_cast<std::size_t>(
            std::strtoull(argv[1], nullptr, 10));
    }
    if (argc >= 3) {
        runs = std::atoi(argv[2]);
    }
    if (samples_per_run == 0 || runs <= 0) {
        std::cerr << "samples and runs must be positive\n";
        return 2;
    }

    void* library = dlopen("./player.so", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::cerr << "dlopen failed: " << dlerror() << '\n';
        return 1;
    }
    dlerror();
    void* symbol = dlsym(library, "moveDecision");
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr) {
        std::cerr << "dlsym failed: " << symbol_error << '\n';
        dlclose(library);
        return 1;
    }
    DecisionFunction decide = nullptr;
    static_assert(sizeof(decide) == sizeof(symbol));
    std::memcpy(&decide, &symbol, sizeof(decide));

    std::array<GameInput, kTurns> turns{};
    for (int round = 0; round < kTurns; ++round) {
        turns[round] = makeTurn(round);
    }

    const auto first_begin = Clock::now();
    const GameOutput first_output = decide(&turns[0]);
    const auto first_end = Clock::now();
    std::uint64_t checksum = static_cast<std::uint64_t>(first_output.k + 1);
    const double first_us =
        std::chrono::duration<double, std::micro>(first_end - first_begin)
            .count();
    std::cout << std::fixed << std::setprecision(3)
              << "first_call_us=" << first_us << '\n';

    for (int warmup = 0; warmup < 2000; ++warmup) {
        const GameOutput output = decide(&turns[warmup % kTurns]);
        checksum = checksum * 1315423911ULL +
                   static_cast<std::uint64_t>(output.actions[warmup % S] + 1 +
                                              output.k * 7 + output.vp * 31);
    }

    std::vector<std::int64_t> samples(samples_per_run);
    for (int run = 1; run <= runs; ++run) {
        checksum ^= static_cast<std::uint64_t>(run);
        for (std::size_t sample = 0; sample < samples_per_run; ++sample) {
            const GameInput& input = turns[sample % kTurns];
            const auto begin = Clock::now();
            const GameOutput output = decide(&input);
            const auto end = Clock::now();
            samples[sample] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                     begin)
                    .count();
            checksum = checksum * 11400714819323198485ULL +
                       static_cast<std::uint64_t>(
                           output.actions[sample % S] + 1 + output.k * 7 +
                           output.order * 17 + output.vp * 31);
        }
        printRun(run, samples, checksum);
    }

    dlclose(library);
    return checksum == 0 ? 3 : 0;
}
