#include "game_api.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

constexpr int kStay = 4;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

std::uint64_t gChecks = 0;

[[noreturn]] void fail(const char* message, int round = -1) {
    std::cerr << "FAIL: " << message;
    if (round >= 0) std::cerr << " (round=" << round << ')';
    std::cerr << '\n';
    std::exit(1);
}

void require(bool condition, const char* message, int round = -1) {
    ++gChecks;
    if (!condition) fail(message, round);
}

bool inside(Position position) {
    return static_cast<unsigned>(position.row) <
               static_cast<unsigned>(GRID_SIZE) &&
           static_cast<unsigned>(position.col) <
               static_cast<unsigned>(GRID_SIZE);
}

bool same(Position first, Position second) {
    return first.row == second.row && first.col == second.col;
}

GameInput baseInput(int round) {
    GameInput input{};
    input.round = round;
    for (auto& row : input.grid) {
        for (int& tile : row) tile = -5;
    }
    input.my_units[0] = Position{0, 0};
    input.my_units[1] = Position{16, 16};
    input.visible_enemies[0] = Position{-1, -1};
    input.visible_enemies[1] = Position{-1, -1};
    for (NpcInfo& npc : input.visible_npcs) {
        npc.id = 0;
        npc.pos = Position{-1, -1};
    }
    input.snapshot.window_begin = -1;
    input.snapshot.window_end = -1;
    return input;
}

void reveal(GameInput& input, Position center, int radius) {
    for (int dr = -radius; dr <= radius; ++dr) {
        for (int dc = -radius; dc <= radius; ++dc) {
            const Position position{center.row + dr, center.col + dc};
            if (inside(position)) input.grid[position.row][position.col] = 0;
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

int validateKnownPath(const GameInput& input, const GameOutput& output,
                      Position watched = Position{-1, -1}) {
    Position positions[2] = {input.my_units[0], input.my_units[1]};
    int visits = 0;
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : 1 - output.order;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            require(inside(next), "boundary collision", input.round);
            require(input.grid[next.row][next.col] >= 0,
                    "entered fog, wall, or bomb", input.round);
            require(!same(next, positions[1 - unit]), "self collision",
                    input.round);
            for (Position enemy : input.visible_enemies) {
                require(!inside(enemy) || !same(next, enemy),
                        "visible-enemy collision", input.round);
            }
            positions[unit] = next;
            visits += same(next, watched);
        }
    }
    return visits;
}

void validateDecision(const GameInput& input) {
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validateKnownPath(input, output);
}

void testFallback() {
    GameOutput output = moveDecision(nullptr);
    validateOutput(output, -1);
    require(output.k == 0, "null fallback k must be zero");
    for (int action : output.actions) {
        require(action == kStay, "null fallback must stay");
    }

    GameInput input = baseInput(-1);
    output = moveDecision(&input);
    for (int action : output.actions) {
        require(action == kStay, "negative-round fallback must stay");
    }
    input = baseInput(0);
    input.my_units[0] = Position{-1, 0};
    output = moveDecision(&input);
    for (int action : output.actions) {
        require(action == kStay, "invalid-position fallback must stay");
    }
}

void testFogAndCorners() {
    validateDecision(baseInput(0));
    GameInput input = baseInput(499);
    input.my_units[0] = Position{16, 0};
    input.my_units[1] = Position{0, 16};
    validateDecision(input);
}

void testGuardAndMainGold() {
    GameInput input = baseInput(21);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{14, 14};
    reveal(input, input.my_units[0], 2);
    reveal(input, input.my_units[1], 2);
    input.grid[8][9] = 100;
    GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    require(output.k == 3, "adjacent guard gold should receive three slots");
    require(validateKnownPath(input, output, Position{8, 9}) >= 2,
            "guard did not re-enter adjacent gold");

    input = baseInput(22);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{10, 10};
    reveal(input, input.my_units[0], 2);
    reveal(input, input.my_units[1], 2);
    input.grid[8][9] = 120;
    output = moveDecision(&input);
    validateOutput(output, input.round);
    require(validateKnownPath(input, output, Position{8, 9}) >= 1,
            "main 25-cell scan missed visible gold");

    input = baseInput(23);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{14, 14};
    reveal(input, input.my_units[0], 2);
    reveal(input, input.my_units[1], 2);
    input.grid[8][8] = 90;
    output = moveDecision(&input);
    validateOutput(output, input.round);
    require(validateKnownPath(input, output, Position{8, 8}) >= 1,
            "guard did not leave and re-enter underfoot gold");
}

void testKnownHazardsAndEnemies() {
    GameInput input = baseInput(101);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{9, 10};
    reveal(input, input.my_units[0], 3);
    reveal(input, input.my_units[1], 3);
    input.grid[7][8] = -3;
    input.grid[8][7] = -1;
    input.grid[9][10] = 20;
    input.grid[9][9] = 80;
    input.visible_enemies[0] = Position{9, 9};
    input.visible_enemies[1] = Position{10, 10};
    validateDecision(input);
}

struct Rng {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;

    std::uint32_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<std::uint32_t>(
            (state * 0x2545f4914f6cdd1dULL) >> 32);
    }

    int range(int upper) {
        return upper <= 1 ? 0 : static_cast<int>(next() %
                                                 static_cast<unsigned>(upper));
    }
};

Position randomPosition(Rng& rng) {
    return Position{rng.range(GRID_SIZE), rng.range(GRID_SIZE)};
}

GameInput randomInput(Rng& rng, int round) {
    GameInput input = baseInput(round);
    input.my_units[0] = randomPosition(rng);
    do {
        input.my_units[1] = randomPosition(rng);
    } while (same(input.my_units[0], input.my_units[1]));

    for (Position unit : input.my_units) {
        for (int dr = -2; dr <= 2; ++dr) {
            for (int dc = -2; dc <= 2; ++dc) {
                const Position position{unit.row + dr, unit.col + dc};
                if (!inside(position)) continue;
                const int roll = rng.range(100);
                input.grid[position.row][position.col] =
                    roll < 8    ? -1
                    : roll < 13 ? -3
                    : roll < 61 ? 0
                                : 1 + rng.range(1000000);
            }
        }
    }
    for (Position unit : input.my_units) {
        input.grid[unit.row][unit.col] = rng.range(5) == 0
                                             ? 1 + rng.range(1000)
                                             : 0;
    }

    Position used[4] = {input.my_units[0], input.my_units[1],
                        Position{-1, -1}, Position{-1, -1}};
    int usedCount = 2;
    for (int enemy = 0; enemy < 2; ++enemy) {
        if (rng.range(3) == 0) continue;
        Position position{};
        bool distinct = false;
        for (int attempt = 0; attempt < 100 && !distinct; ++attempt) {
            position = randomPosition(rng);
            distinct = true;
            for (int index = 0; index < usedCount; ++index) {
                if (same(position, used[index])) distinct = false;
            }
        }
        if (!distinct) continue;
        input.visible_enemies[enemy] = position;
        input.grid[position.row][position.col] = 0;
        used[usedCount++] = position;
    }

    input.my_units_gold[0] = rng.range(100000);
    input.my_units_gold[1] = rng.range(100000);
    input.gold_opp = rng.range(200000);
    input.num_visible_npcs = rng.range(MAX_NPCS + 1);
    for (int npc = 0; npc < input.num_visible_npcs; ++npc) {
        input.visible_npcs[npc].id = npc + 1;
        input.visible_npcs[npc].pos = randomPosition(rng);
    }
    input.snapshot_valid = rng.range(2);
    input.snapshot.window_begin = input.snapshot_valid ? round - round % 5 : -1;
    input.snapshot.window_end = input.snapshot_valid ? round : -1;
    for (int region = 0; region < REGION_COUNT; ++region) {
        RegionStat& stat = input.snapshot.regions[region];
        stat.id = region + 1;
        stat.enter = rng.range(50);
        stat.leave = rng.range(50);
        stat.gold_generated = rng.range(10000);
        stat.gold_collected = rng.range(10000);
        stat.gold_remaining = rng.range(10000);
        stat.occupants = rng.range(12);
    }
    return input;
}

void testRoundsAndFuzz(std::uint64_t iterations) {
    Rng rng;
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        int round = static_cast<int>(iteration % 500U);
        if (iteration % 997U == 0) round = 0;
        if (iteration % 1291U == 0) round = rng.range(500);
        validateDecision(randomInput(rng, round));
    }
    validateDecision(randomInput(rng, 0));
    validateDecision(randomInput(rng, 499));
    validateDecision(randomInput(rng, 7));
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t fuzzIterations = 50000;
    if (argc == 3 && std::strcmp(argv[1], "--fuzz") == 0) {
        fuzzIterations = std::strtoull(argv[2], nullptr, 10);
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--fuzz ITERATIONS]\n";
        return 2;
    }

    testFallback();
    testFogAndCorners();
    testGuardAndMainGold();
    testKnownHazardsAndEnemies();
    testRoundsAndFuzz(fuzzIterations);
    std::cout << "PASS: " << gChecks << " assertions, " << fuzzIterations
              << " deterministic fuzz inputs\n";
    return 0;
}
