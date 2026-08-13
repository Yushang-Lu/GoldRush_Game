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

DecisionFunction loadDecision(const char* path, void*& library) {
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::cerr << "dlopen failed for " << path << ": " << dlerror()
                  << '\n';
        return nullptr;
    }
    dlerror();
    void* symbol = dlsym(library, "moveDecision");
    const char* error = dlerror();
    if (error != nullptr) {
        std::cerr << "dlsym failed for " << path << ": " << error << '\n';
        dlclose(library);
        library = nullptr;
        return nullptr;
    }
    DecisionFunction result = nullptr;
    static_assert(sizeof(result) == sizeof(symbol));
    std::memcpy(&result, &symbol, sizeof(result));
    return result;
}

double measureP90(DecisionFunction decide,
                  const std::array<GameInput, kTurns>& turns,
                  std::size_t samples, std::uint64_t& checksum,
                  std::size_t offset) {
    constexpr int kWarmupGames = 4;
    for (int game = 0; game < kWarmupGames; ++game) {
        for (int round = 0; round < kTurns; ++round) {
            const GameOutput output = decide(&turns[round]);
            checksum = checksum * 1315423911ULL +
                       static_cast<std::uint64_t>(
                           output.actions[(round + game) % S] + 1 +
                           output.k * 7 + output.vp * 31);
        }
    }
    std::vector<std::int64_t> values(samples);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        const std::size_t index = sample + offset;
        const GameInput& input = turns[index % kTurns];
        const auto begin = Clock::now();
        const GameOutput output = decide(&input);
        const auto end = Clock::now();
        values[sample] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             end - begin).count();
        checksum = checksum * 1315423911ULL +
                   static_cast<std::uint64_t>(output.actions[index % S] + 1 +
                                              output.k * 7 + output.vp * 31);
    }
    std::sort(values.begin(), values.end());
    return percentile(values, 0.90);
}

double measureComparison(DecisionFunction first, DecisionFunction second,
                         const std::array<GameInput, kTurns>& turns,
                         std::size_t samples, std::uint64_t& checksum,
                         std::size_t offset, double& second_p90) {
    const double first_p90 = measureP90(
        first, turns, samples, checksum, offset);
    second_p90 = measureP90(
        second, turns, samples, checksum, offset + kTurns / 2);
    return first_p90;
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
    if (argc > 5) {
        std::cerr << "usage: " << argv[0]
                  << " [SAMPLES [RUNS [BASELINE_SO [MAX_RATIO]]]]\n";
        return 2;
    }

    void* library = nullptr;
    DecisionFunction decide = loadDecision("./player.so", library);
    if (decide == nullptr) {
        return 1;
    }

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

    if (argc >= 4) {
        void* baseline_library = nullptr;
        DecisionFunction baseline = loadDecision(argv[3], baseline_library);
        if (baseline == nullptr) {
            dlclose(library);
            return 1;
        }
        const double required_ratio = argc >= 5 ? std::strtod(argv[4], nullptr)
                                                : 0.05;
        double candidate_p90_first = 0.0;
        const double baseline_p90_first = measureComparison(
            baseline, decide, turns, samples_per_run, checksum, 0,
            candidate_p90_first);
        double baseline_p90_second = 0.0;
        const double candidate_p90_second = measureComparison(
            decide, baseline, turns, samples_per_run, checksum, kTurns / 4,
            baseline_p90_second);
        const double baseline_p90 =
            (baseline_p90_first + baseline_p90_second) / 2.0;
        const double candidate_p90 =
            (candidate_p90_first + candidate_p90_second) / 2.0;
        const double ratio = candidate_p90 / baseline_p90;
        std::cout << "comparison baseline_p90_us=" << baseline_p90
                  << " candidate_p90_us=" << candidate_p90
                  << " ratio=" << ratio
                  << " required_ratio=" << required_ratio << '\n';
        dlclose(baseline_library);
        if (ratio > required_ratio) {
            dlclose(library);
            return 4;
        }
    }
    dlclose(library);
    return checksum == 0 ? 3 : 0;
}
