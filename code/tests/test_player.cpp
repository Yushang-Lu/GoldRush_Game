#include "game_api.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

constexpr int kStay = 4;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

std::uint64_t g_checks = 0;

[[noreturn]] void fail(const char* message, int round = -1) {
    std::cerr << "FAIL: " << message;
    if (round >= 0) {
        std::cerr << " (round=" << round << ')';
    }
    std::cerr << '\n';
    std::exit(1);
}

void require(bool condition, const char* message, int round = -1) {
    ++g_checks;
    if (!condition) {
        fail(message, round);
    }
}

bool validPosition(const Position& pos) {
    return pos.row >= 0 && pos.row < GRID_SIZE && pos.col >= 0 &&
           pos.col < GRID_SIZE;
}

GameInput baseInput(int round) {
    GameInput input{};
    input.round = round;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            input.grid[row][col] = -5;
        }
    }
    input.my_units[0] = Position{0, 0};
    input.my_units[1] = Position{GRID_SIZE - 1, GRID_SIZE - 1};
    input.visible_enemies[0] = Position{-1, -1};
    input.visible_enemies[1] = Position{-1, -1};
    input.num_visible_npcs = 0;
    for (int i = 0; i < MAX_NPCS; ++i) {
        input.visible_npcs[i].id = 0;
        input.visible_npcs[i].pos = Position{-1, -1};
    }
    input.snapshot_valid = 0;
    input.snapshot.window_begin = -1;
    input.snapshot.window_end = -1;
    for (int i = 0; i < REGION_COUNT; ++i) {
        input.snapshot.regions[i].id = i + 1;
    }
    return input;
}

void reveal(GameInput& input, Position center, int radius) {
    for (int dr = -radius; dr <= radius; ++dr) {
        const int row = center.row + dr;
        if (row < 0 || row >= GRID_SIZE) {
            continue;
        }
        for (int dc = -radius; dc <= radius; ++dc) {
            const int col = center.col + dc;
            if (col >= 0 && col < GRID_SIZE && input.grid[row][col] == -5) {
                input.grid[row][col] = 0;
            }
        }
    }
}

void validateOutput(const GameOutput& output, int round) {
    for (int action : output.actions) {
        require(action >= 0 && action <= kStay, "action outside [0,4]",
                round);
    }
    require(output.k >= 0 && output.k <= S, "k outside [0,6]", round);
    require(output.order == 0 || output.order == 1,
            "order outside {0,1}", round);
    require(output.vp >= 0 && output.vp <= 2, "vp outside [0,2]", round);
}

void validateKnownPath(const GameInput& input, const GameOutput& output) {
    Position positions[2] = {input.my_units[0], input.my_units[1]};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : 1 - output.order;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) {
                continue;
            }
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            require(validPosition(next), "strategy emitted boundary collision",
                    input.round);
            require(input.grid[next.row][next.col] != -1,
                    "strategy emitted known wall collision", input.round);
            require(next.row != positions[1 - unit].row ||
                        next.col != positions[1 - unit].col,
                    "strategy emitted self collision", input.round);
            for (int enemy = 0; enemy < 2; ++enemy) {
                if (validPosition(input.visible_enemies[enemy])) {
                    require(next.row != input.visible_enemies[enemy].row ||
                                next.col != input.visible_enemies[enemy].col,
                            "strategy emitted visible-enemy collision",
                            input.round);
                }
            }
            positions[unit] = next;
        }
    }
}

bool visitsCell(const GameInput& input, const GameOutput& output,
                Position target) {
    Position positions[2] = {input.my_units[0], input.my_units[1]};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : 1 - output.order;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) {
                continue;
            }
            Position next{positions[unit].row + kDr[action],
                          positions[unit].col + kDc[action]};
            if (!validPosition(next) || input.grid[next.row][next.col] == -1 ||
                (next.row == positions[1 - unit].row &&
                 next.col == positions[1 - unit].col)) {
                continue;
            }
            positions[unit] = next;
            if (next.row == target.row && next.col == target.col) {
                return true;
            }
        }
    }
    return false;
}

bool unitVisitsCell(const GameInput& input, const GameOutput& output,
                    int watched_unit, Position target) {
    Position positions[2] = {input.my_units[0], input.my_units[1]};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : 1 - output.order;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) {
                continue;
            }
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            if (!validPosition(next) || input.grid[next.row][next.col] == -1 ||
                (next.row == positions[1 - unit].row &&
                 next.col == positions[1 - unit].col)) {
                continue;
            }
            positions[unit] = next;
            if (unit == watched_unit && next.row == target.row &&
                next.col == target.col) {
                return true;
            }
        }
    }
    return false;
}

struct Rng {
    std::uint64_t state = 0xd1b54a32d192ed03ULL;

    std::uint32_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<std::uint32_t>(
            (state * 0x2545f4914f6cdd1dULL) >> 32);
    }

    int range(int upper) {
        return upper <= 1 ? 0 : static_cast<int>(next() % upper);
    }
};

Position randomDistinctPosition(Rng& rng, const Position* forbidden,
                                int forbidden_count) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        Position candidate{rng.range(GRID_SIZE), rng.range(GRID_SIZE)};
        bool distinct = true;
        for (int i = 0; i < forbidden_count; ++i) {
            if (candidate.row == forbidden[i].row &&
                candidate.col == forbidden[i].col) {
                distinct = false;
            }
        }
        if (distinct) {
            return candidate;
        }
    }
    fail("could not generate distinct position");
}

GameInput randomInput(Rng& rng, int round) {
    GameInput input = baseInput(round);
    input.my_units[0] = randomDistinctPosition(rng, nullptr, 0);
    input.my_units[1] =
        randomDistinctPosition(rng, input.my_units, 1);
    input.my_units_gold[0] = rng.range(50000);
    input.my_units_gold[1] = rng.range(50000);
    input.gold_opp = rng.range(100000);

    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int roll = rng.range(100);
            if (roll < 42) {
                input.grid[row][col] = -5;
            } else if (roll < 49) {
                input.grid[row][col] = -1;
            } else if (roll < 54) {
                input.grid[row][col] = -3;
            } else if (roll < 78) {
                input.grid[row][col] = 0;
            } else {
                input.grid[row][col] = 1 + rng.range(5000);
            }
        }
    }
    input.grid[input.my_units[0].row][input.my_units[0].col] = 0;
    input.grid[input.my_units[1].row][input.my_units[1].col] = 0;

    Position forbidden[4] = {input.my_units[0], input.my_units[1],
                             Position{-1, -1}, Position{-1, -1}};
    int forbidden_count = 2;
    for (int enemy = 0; enemy < 2; ++enemy) {
        if (rng.range(3) == 0) {
            input.visible_enemies[enemy] = Position{-1, -1};
            continue;
        }
        const Position pos =
            randomDistinctPosition(rng, forbidden, forbidden_count);
        input.visible_enemies[enemy] = pos;
        forbidden[forbidden_count++] = pos;
        input.grid[pos.row][pos.col] = 0;
    }

    input.num_visible_npcs = rng.range(MAX_NPCS + 1);
    for (int npc = 0; npc < input.num_visible_npcs; ++npc) {
        input.visible_npcs[npc].id = npc + 1;
        input.visible_npcs[npc].pos =
            Position{rng.range(GRID_SIZE), rng.range(GRID_SIZE)};
    }

    input.snapshot_valid = rng.range(2);
    if (input.snapshot_valid == 1) {
        input.snapshot.window_begin = std::max(0, round - 5);
        input.snapshot.window_end = round;
        for (int region = 0; region < REGION_COUNT; ++region) {
            RegionStat& stat = input.snapshot.regions[region];
            stat.id = region + 1;
            stat.enter = rng.range(40);
            stat.leave = rng.range(40);
            stat.gold_generated = rng.range(10000);
            stat.gold_collected = rng.range(10000);
            stat.gold_remaining = rng.range(10000);
            stat.occupants = rng.range(12);
        }
    }
    return input;
}

void testFallback() {
    const GameOutput null_output = moveDecision(nullptr);
    validateOutput(null_output, -1);
    require(null_output.k == 0, "null input fallback k is not zero");
    for (int action : null_output.actions) {
        require(action == kStay, "null input fallback must explicitly stay");
    }

    GameInput invalid = baseInput(-1);
    const GameOutput negative_round = moveDecision(&invalid);
    validateOutput(negative_round, -1);
    for (int action : negative_round.actions) {
        require(action == kStay,
                "negative-round fallback must explicitly stay");
    }

    invalid = baseInput(0);
    invalid.my_units[0] = Position{-1, -1};
    const GameOutput invalid_position = moveDecision(&invalid);
    validateOutput(invalid_position, 0);
    for (int action : invalid_position.actions) {
        require(action == kStay,
                "invalid-position fallback must explicitly stay");
    }
}

void testAllFogAndCorners() {
    GameInput input = baseInput(0);
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
}

void testAdjacentGold() {
    GameInput input = baseInput(0);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{16, 16};
    reveal(input, input.my_units[0], 3);
    reveal(input, input.my_units[1], 2);
    input.grid[8][9] = 100;
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
    require(visitsCell(input, output, Position{8, 9}),
            "strategy ignored an uncontested adjacent high-value coin");
}

void testObstaclesBombsNpcsEnemiesAndSnapshots() {
    GameInput input = baseInput(0);
    input.my_units[0] = Position{8, 6};
    input.my_units[1] = Position{8, 10};
    input.my_units_gold[0] = 1000;
    input.my_units_gold[1] = 2000;
    reveal(input, input.my_units[0], 4);
    reveal(input, input.my_units[1], 4);
    for (int row = 5; row <= 11; ++row) {
        input.grid[row][8] = -1;
    }
    input.grid[7][6] = -3;
    input.grid[9][10] = 75;
    input.visible_enemies[0] = Position{8, 7};
    input.visible_enemies[1] = Position{7, 10};
    input.num_visible_npcs = MAX_NPCS;
    for (int npc = 0; npc < MAX_NPCS; ++npc) {
        input.visible_npcs[npc].id = npc + 1;
        input.visible_npcs[npc].pos =
            npc < 4 ? Position{9, 10} : Position{10, npc + 2};
    }
    input.snapshot_valid = 1;
    input.snapshot.window_begin = 0;
    input.snapshot.window_end = 0;
    for (int region = 0; region < REGION_COUNT; ++region) {
        RegionStat& stat = input.snapshot.regions[region];
        stat.id = region + 1;
        stat.enter = region;
        stat.leave = region + 1;
        stat.gold_generated = 100 + region * 10;
        stat.gold_collected = 50 + region * 5;
        stat.gold_remaining = 25 + region * 7;
        stat.occupants = region;
    }
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
}

void testRememberedBombAvoidanceAndRefresh() {
    GameInput input = baseInput(0);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{16, 16};
    input.my_units_gold[0] = 1000;
    reveal(input, input.my_units[0], 3);
    reveal(input, input.my_units[1], 2);
    input.grid[8][9] = -3;
    GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);

    input.round = 1;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            input.grid[row][col] = -5;
        }
    }
    output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
    require(!unitVisitsCell(input, output, 0, Position{8, 9}),
            "high-gold unit entered a remembered bomb hidden by fog");

    input.round = 20;
    output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
}

void testLargeGoldDoesNotOverflow() {
    GameInput input = baseInput(0);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{16, 16};
    reveal(input, input.my_units[0], 3);
    reveal(input, input.my_units[1], 2);
    input.grid[8][9] = 1000000;
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
    require(visitsCell(input, output, Position{8, 9}),
            "strategy overflowed while evaluating maximum clamped gold");
}

void testRoundSequences() {
    GameInput input = baseInput(0);
    input.my_units[0] = Position{1, 1};
    input.my_units[1] = Position{15, 15};
    for (int round = 0; round < 500; ++round) {
        input.round = round;
        for (int row = 0; row < GRID_SIZE; ++row) {
            for (int col = 0; col < GRID_SIZE; ++col) {
                input.grid[row][col] = -5;
            }
        }
        reveal(input, input.my_units[0], 2);
        reveal(input, input.my_units[1], 2);
        input.grid[(round * 5 + 3) % GRID_SIZE]
                  [(round * 7 + 4) % GRID_SIZE] = 1 + round % 100;
        input.snapshot_valid = round % 5 == 0 ? 1 : 0;
        const GameOutput output = moveDecision(&input);
        validateOutput(output, round);
        validateKnownPath(input, output);
    }

    input.round = 0;
    GameOutput output = moveDecision(&input);
    validateOutput(output, 0);
    validateKnownPath(input, output);

    input.round = 271;
    output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
    input.round = 7;
    output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
}

void testFuzz(std::uint64_t iterations) {
    Rng rng;
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        int round = static_cast<int>(iteration % 500);
        if (iteration % 997 == 0) {
            round = 0;
        } else if (iteration % 1291 == 0) {
            round = rng.range(500);
        }
        GameInput input = randomInput(rng, round);
        const GameOutput output = moveDecision(&input);
        validateOutput(output, round);
        validateKnownPath(input, output);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t fuzz_iterations = 50000;
    if (argc == 3 && std::strcmp(argv[1], "--fuzz") == 0) {
        fuzz_iterations = std::strtoull(argv[2], nullptr, 10);
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--fuzz ITERATIONS]\n";
        return 2;
    }

    testFallback();
    testAllFogAndCorners();
    testAdjacentGold();
    testObstaclesBombsNpcsEnemiesAndSnapshots();
    testRememberedBombAvoidanceAndRefresh();
    testLargeGoldDoesNotOverflow();
    testRoundSequences();
    testFuzz(fuzz_iterations);
    std::cout << "PASS: " << g_checks << " assertions, " << fuzz_iterations
              << " deterministic fuzz inputs\n";
    return 0;
}
