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

bool validatePath(const GameInput& input, const GameOutput& output,
                  Position watched = Position{-1, -1}) {
    Position positions[2] = {input.my_units[0], input.my_units[1]};
    bool visited = false;
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            require(inside(next), "boundary move", input.round);
            require(input.grid[next.row][next.col] >= 0,
                    "entered fog, wall, or bomb", input.round);
            require(!same(next, positions[unit ^ 1]), "self collision",
                    input.round);
            for (Position enemy : input.visible_enemies) {
                require(!inside(enemy) || !same(next, enemy),
                        "visible enemy collision", input.round);
            }
            positions[unit] = next;
            visited |= same(next, watched);
        }
    }
    return visited;
}

#ifdef TEST_RISK_AWARE
bool crowdedAt(const GameInput& input, int row, int col) {
    int limit = input.num_visible_npcs;
    if (limit < 0) limit = 0;
    if (limit > MAX_NPCS) limit = MAX_NPCS;
    int count = 0;
    for (int index = 0; index < limit; ++index) {
        const NpcInfo& npc = input.visible_npcs[index];
        count += npc.id != 0 && npc.pos.row == row && npc.pos.col == col;
    }
    return count >= 3;
}

void validatePrefixSafety(const GameInput& input, const GameOutput& output) {
    for (int unit = 0; unit < 2; ++unit) {
#ifdef TEST_STATIC_BLOCK_SAFETY
        bool nearbyEnemy = false;
        for (Position enemy : input.visible_enemies) {
            nearbyEnemy |= inside(enemy) &&
                           std::abs(enemy.row - input.my_units[unit].row) +
                                   std::abs(enemy.col -
                                            input.my_units[unit].col) <=
                               2;
        }
        if (!nearbyEnemy) continue;
        int states[64] = {input.my_units[unit].row * GRID_SIZE +
                          input.my_units[unit].col};
        int stateCount = 1;
        int candidates[64];
        int candidateCount = 0;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int actionIndex = begin; actionIndex < end; ++actionIndex) {
            const int action = output.actions[actionIndex];
            if (action == kStay) continue;
            int nextStates[64];
            int nextCount = 0;
            for (int index = 0; index < stateCount; ++index) {
                const int row = states[index] / GRID_SIZE;
                const int col = states[index] - row * GRID_SIZE;
                const Position next{row + kDr[action], col + kDc[action]};
                if (inside(next)) {
                    const int attempted = next.row * GRID_SIZE + next.col;
                    bool known = false;
                    for (int old = 0; old < candidateCount; ++old) {
                        known |= candidates[old] == attempted;
                    }
                    if (!known) candidates[candidateCount++] = attempted;
                }
                const int moved = inside(next)
                                      ? next.row * GRID_SIZE + next.col
                                      : states[index];
                const int alternatives[2] = {states[index], moved};
                for (int alternative : alternatives) {
                    bool duplicate = false;
                    for (int old = 0; old < nextCount; ++old) {
                        duplicate |= nextStates[old] == alternative;
                    }
                    if (!duplicate) nextStates[nextCount++] = alternative;
                }
            }
            stateCount = nextCount;
            for (int index = 0; index < stateCount; ++index) {
                states[index] = nextStates[index];
            }
        }
        for (int candidateIndex = -1; candidateIndex < candidateCount;
             ++candidateIndex) {
            const int blocked = candidateIndex < 0
                                    ? -1
                                    : candidates[candidateIndex];
            Position position = input.my_units[unit];
            for (int actionIndex = begin; actionIndex < end; ++actionIndex) {
                const int action = output.actions[actionIndex];
                if (action == kStay) continue;
                const Position next{position.row + kDr[action],
                                    position.col + kDc[action]};
                require(inside(next), "static-block boundary move",
                        input.round);
                if (next.row * GRID_SIZE + next.col == blocked) continue;
                require(input.grid[next.row][next.col] >= 0,
                        "static-block entered fog, wall, or bomb",
                        input.round);
                require(!crowdedAt(input, next.row, next.col),
                        "static-block entered crowded NPC cell",
                        input.round);
                for (Position enemy : input.visible_enemies) {
                    require(!inside(enemy) || !same(next, enemy),
                            "static-block visible enemy collision",
                            input.round);
                }
                position = next;
            }
        }
#else
        int states[8] = {input.my_units[unit].row * GRID_SIZE +
                         input.my_units[unit].col};
        int stateCount = 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int actionIndex = begin; actionIndex < end; ++actionIndex) {
            const int action = output.actions[actionIndex];
            if (action == kStay) continue;
            int nextStates[8];
            int nextCount = 0;
            for (int index = 0; index < stateCount; ++index) {
                const int row = states[index] / GRID_SIZE;
                const int col = states[index] - row * GRID_SIZE;
                const Position next{row + kDr[action], col + kDc[action]};
                require(inside(next), "blocked-prefix boundary move",
                        input.round);
                require(input.grid[next.row][next.col] >= 0,
                        "blocked-prefix entered fog, wall, or bomb",
                        input.round);
                require(!crowdedAt(input, next.row, next.col),
                        "blocked-prefix entered crowded NPC cell",
                        input.round);
                for (Position enemy : input.visible_enemies) {
                    require(!inside(enemy) || !same(next, enemy),
                            "blocked-prefix visible enemy collision",
                            input.round);
                }
                const int moved = next.row * GRID_SIZE + next.col;
                const int alternatives[2] = {states[index], moved};
                for (int alternative : alternatives) {
                    bool duplicate = false;
                    for (int old = 0; old < nextCount; ++old) {
                        duplicate |= nextStates[old] == alternative;
                    }
                    if (!duplicate) nextStates[nextCount++] = alternative;
                }
            }
            stateCount = nextCount;
            for (int index = 0; index < stateCount; ++index) {
                states[index] = nextStates[index];
            }
        }
#endif
    }
}
#endif

void validateDecision(const GameInput& input) {
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    validatePath(input, output);
#ifdef TEST_RISK_AWARE
    validatePrefixSafety(input, output);
#endif
}

void testFallbackAndFixedShape() {
    const GameOutput nullOutput = moveDecision(nullptr);
    validateOutput(nullOutput, -1);
    require(nullOutput.k == 0, "null fallback k");
    for (int action : nullOutput.actions) {
        require(action == kStay, "null fallback action");
    }

    GameInput input = baseInput(-1);
    GameOutput output = moveDecision(&input);
    for (int action : output.actions) {
        require(action == kStay, "negative-round fallback action");
    }

    input = baseInput(4);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{12, 12};
    reveal(input, input.my_units[0], 3);
    reveal(input, input.my_units[1], 3);
    output = moveDecision(&input);
    validateOutput(output, input.round);
    validatePath(input, output);
#ifndef TEST_DYNAMIC_ALLOCATION
    require(output.k == 3, "release strategy must use 3+3 allocation");
#endif
    require(output.vp == 0, "release strategy must not buy vision");
}

void testGoldAndMovement() {
    GameInput input = baseInput(8);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{14, 14};
    reveal(input, input.my_units[0], 3);
    reveal(input, input.my_units[1], 2);
    input.grid[8][9] = 100;
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    require(validatePath(input, output, Position{8, 9}),
            "ignored adjacent gold");

    input = baseInput(9);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{12, 12};
    reveal(input, input.my_units[0], 4);
    reveal(input, input.my_units[1], 4);
    const GameOutput phaseOutput = moveDecision(&input);
    validateOutput(phaseOutput, input.round);
    validatePath(input, phaseOutput);
    int moves = 0;
    for (int action : phaseOutput.actions) moves += action != kStay;
    require(moves >= 4, "phase walker left too many steps idle");
}

void testKnownHazards() {
    GameInput input = baseInput(101);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{10, 10};
    reveal(input, input.my_units[0], 4);
    reveal(input, input.my_units[1], 4);
    input.grid[7][8] = -3;
    input.grid[8][7] = -1;
    input.grid[9][8] = 80;
    input.visible_enemies[0] = Position{8, 9};
    input.visible_enemies[1] = Position{10, 9};
    validateDecision(input);

    validateDecision(baseInput(0));
    input = baseInput(499);
    input.my_units[0] = Position{16, 0};
    input.my_units[1] = Position{0, 16};
    validateDecision(input);
}

#ifdef TEST_RISK_AWARE
void testCrowdedGoldAvoidance() {
    GameInput input = baseInput(31);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{14, 14};
    reveal(input, input.my_units[0], 4);
    reveal(input, input.my_units[1], 3);
    input.grid[8][9] = 1000;
    input.num_visible_npcs = 3;
    for (int index = 0; index < 3; ++index) {
        input.visible_npcs[index].id = index + 1;
        input.visible_npcs[index].pos = Position{8, 9};
    }
    const GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);
    require(!validatePath(input, output, Position{8, 9}),
            "entered known 3-NPC cell");
    validatePrefixSafety(input, output);
}
#endif

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
    for (Position unit : input.my_units) reveal(input, unit, 3);
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            if (input.grid[row][col] == -5) continue;
            const int roll = rng.range(100);
            input.grid[row][col] = roll < 8    ? -1
                                   : roll < 13 ? -3
                                   : roll < 66 ? 0
                                               : 1 + rng.range(1000000);
        }
    }
    for (Position unit : input.my_units) input.grid[unit.row][unit.col] = 0;

    Position used[4] = {input.my_units[0], input.my_units[1],
                        Position{-1, -1}, Position{-1, -1}};
    int usedCount = 2;
    for (int enemy = 0; enemy < 2; ++enemy) {
        if (rng.range(3) == 0) continue;
        Position value{};
        bool distinct = false;
        for (int attempt = 0; attempt < 100 && !distinct; ++attempt) {
            value = randomPosition(rng);
            distinct = true;
            for (int index = 0; index < usedCount; ++index) {
                distinct &= !same(value, used[index]);
            }
        }
        if (distinct) {
            input.visible_enemies[enemy] = value;
            input.grid[value.row][value.col] = 0;
            used[usedCount++] = value;
        }
    }
    input.num_visible_npcs = rng.range(MAX_NPCS + 1);
    for (int npc = 0; npc < input.num_visible_npcs; ++npc) {
        input.visible_npcs[npc].id = npc + 1;
        input.visible_npcs[npc].pos = randomPosition(rng);
    }
    input.my_units_gold[0] = rng.range(100000);
    input.my_units_gold[1] = rng.range(100000);
    input.gold_opp = rng.range(200000);
    input.snapshot_valid = rng.range(2);
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
    testFallbackAndFixedShape();
    testGoldAndMovement();
    testKnownHazards();
#ifdef TEST_RISK_AWARE
    testCrowdedGoldAvoidance();
#endif
    testRoundsAndFuzz(fuzzIterations);
    std::cout << "PASS: " << gChecks << " assertions, " << fuzzIterations
              << " deterministic fuzz inputs\n";
    return 0;
}
