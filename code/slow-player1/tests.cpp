#include "game_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

constexpr int kStay = 4;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};
constexpr const char* kMaze[GRID_SIZE] = {
    "..#...#...#...#..", ".##.#.##.##.#.##.", "....#.......#....",
    ".#######.#######.", ".......#.#.......", "######.#.#.######",
    ".....#.....#.....", ".###.###.###.###.", "...#.........#...",
    ".#.#.###.###.#.#.", ".#.#.#.....#.#.#.", ".#.#.#.#.#.#.#.#.",
    ".#...#.#.#.#...#.", ".###.#.###.#.###.", ".................",
    ".###.###.###.###.", "..#...#...#...#.."};

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
        for (int& value : row) value = -5;
    }
    input.my_units[0] = Position{0, 0};
    input.my_units[1] = Position{16, 16};
    input.visible_enemies[0] = Position{-1, -1};
    input.visible_enemies[1] = Position{-1, -1};
    input.num_visible_npcs = 0;
    for (NpcInfo& npc : input.visible_npcs) {
        npc.id = 0;
        npc.pos = Position{-1, -1};
    }
    input.snapshot_valid = 0;
    input.snapshot.window_begin = -1;
    input.snapshot.window_end = -1;
    return input;
}

void validateOutput(const GameOutput& output, int round) {
    for (int action : output.actions) {
        require(action >= 0 && action <= 4, "action outside [0,4]", round);
    }
    require(output.k >= 0 && output.k <= S, "k outside [0,6]", round);
    require(output.order == 0 || output.order == 1,
            "order outside {0,1}", round);
    require(output.vp >= 0 && output.vp <= 2, "vp outside [0,2]", round);
}

void reveal(GameInput& input, Position center, int radius,
            const bool walls[GRID_SIZE][GRID_SIZE],
            const int gold[GRID_SIZE][GRID_SIZE],
            const bool bombs[GRID_SIZE][GRID_SIZE]) {
    for (int dr = -radius; dr <= radius; ++dr) {
        for (int dc = -radius; dc <= radius; ++dc) {
            const Position position{center.row + dr, center.col + dc};
            if (!inside(position)) continue;
            if (walls[position.row][position.col]) {
                input.grid[position.row][position.col] = -1;
            } else if (bombs[position.row][position.col]) {
                input.grid[position.row][position.col] = -3;
            } else {
                input.grid[position.row][position.col] =
                    gold[position.row][position.col];
            }
        }
    }
}

struct Applied {
    Position positions[2];
    int held[2];
};

Applied applyOutput(const GameInput& input, const GameOutput& output,
                    const bool walls[GRID_SIZE][GRID_SIZE],
                    int gold[GRID_SIZE][GRID_SIZE],
                    bool bombs[GRID_SIZE][GRID_SIZE]) {
    Applied applied{{input.my_units[0], input.my_units[1]},
                    {std::max(0, std::min(100000000, input.my_units_gold[0])),
                     std::max(0, std::min(100000000,
                                          input.my_units_gold[1]))}};
    int npcCounts[GRID_SIZE][GRID_SIZE]{};
    const int npcCount = input.num_visible_npcs < 0
                             ? 0
                             : input.num_visible_npcs > MAX_NPCS
                                   ? MAX_NPCS
                                   : input.num_visible_npcs;
    for (int index = 0; index < npcCount; ++index) {
        const Position position = input.visible_npcs[index].pos;
        if (input.visible_npcs[index].id != 0 && inside(position)) {
            ++npcCounts[position.row][position.col];
        }
    }
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{applied.positions[unit].row + kDr[action],
                                applied.positions[unit].col + kDc[action]};
            if (!inside(next) || walls[next.row][next.col] ||
                same(next, applied.positions[unit ^ 1])) {
                continue;
            }
            bool blockedByEnemy = false;
            for (Position enemy : input.visible_enemies) {
                blockedByEnemy |= inside(enemy) && same(next, enemy);
            }
            if (blockedByEnemy) continue;
            applied.positions[unit] = next;
            if (gold[next.row][next.col] > 0) {
                const int value = gold[next.row][next.col];
                const int pickup = static_cast<int>(
                    (static_cast<std::int64_t>(value) * 65 + 99) / 100);
                gold[next.row][next.col] -= pickup;
                const std::int64_t total =
                    static_cast<std::int64_t>(applied.held[unit]) + pickup;
                applied.held[unit] = static_cast<int>(
                    std::min<std::int64_t>(100000000, total));
            }
            if (bombs[next.row][next.col]) {
                applied.held[unit] -= (applied.held[unit] + 9) / 10;
                bombs[next.row][next.col] = false;
            }
            if (npcCounts[next.row][next.col] >= 3) {
                applied.held[unit] -= (applied.held[unit] + 19) / 20;
            }
        }
    }
    require(!same(applied.positions[0], applied.positions[1]),
            "own units overlapped after simulation", input.round);
    return applied;
}

void fillSnapshot(GameInput& input, bool valid = true) {
    input.snapshot_valid = 1;
    input.snapshot.window_begin = input.round - 5;
    input.snapshot.window_end = input.round - 1;
    for (int index = 0; index < REGION_COUNT; ++index) {
        input.snapshot.regions[index] = RegionStat{
            valid ? index + 1 : 1,
            index * 2,
            index,
            index == 0 ? 45 : (index == 2 ? 104 : 8),
            12 + index,
            20 + index * 17,
            index + 1};
    }
}

void testFallbacks() {
    GameOutput output = moveDecision(nullptr);
    validateOutput(output, -1);
    require(output.k == 0, "null fallback split");
    for (int action : output.actions) {
        require(action == kStay, "null fallback action");
    }

    GameInput input = baseInput(-1);
    output = moveDecision(&input);
    for (int action : output.actions) {
        require(action == kStay, "negative-round fallback action");
    }
    input = baseInput(0);
    input.my_units[0] = Position{-1, 0};
    output = moveDecision(&input);
    for (int action : output.actions) {
        require(action == kStay, "invalid-position fallback action");
    }
    input = baseInput(0);
    input.my_units[1] = input.my_units[0];
    output = moveDecision(&input);
    for (int action : output.actions) {
        require(action == kStay, "overlap fallback action");
    }
}

void testFeatureMatrix() {
    bool walls[GRID_SIZE][GRID_SIZE]{};
    bool bombs[GRID_SIZE][GRID_SIZE]{};
    int gold[GRID_SIZE][GRID_SIZE]{};
    for (int npcCount = 0; npcCount <= MAX_NPCS; ++npcCount) {
        GameInput input = baseInput(0);
        input.my_units[0] = Position{8, 7};
        input.my_units[1] = Position{8, 10};
        gold[7][7] = 1;
        gold[8][8] = 100000000;
        gold[9][10] = 37;
        walls[7][8] = true;
        bombs[8][9] = true;
        reveal(input, input.my_units[0], 4, walls, gold, bombs);
        reveal(input, input.my_units[1], 4, walls, gold, bombs);
        input.my_units_gold[0] = std::numeric_limits<int>::max();
        input.my_units_gold[1] = 12345;
        input.gold_opp = std::numeric_limits<int>::max();
        input.visible_enemies[0] = Position{7, 10};
        input.num_visible_npcs = npcCount;
        for (int index = 0; index < npcCount; ++index) {
            input.visible_npcs[index].id = 100 + index;
            input.visible_npcs[index].pos =
                index < 3 ? Position{8, 8}
                          : Position{9 + index % 2, 9 + index % 3};
        }
        fillSnapshot(input, npcCount != MAX_NPCS);
        const GameOutput output = moveDecision(&input);
        validateOutput(output, input.round);
        (void)applyOutput(input, output, walls, gold, bombs);
    }
}

void mazeWalls(bool walls[GRID_SIZE][GRID_SIZE]) {
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            walls[row][col] = kMaze[row][col] == '#';
        }
    }
}

bool visibleFrom(Position center, Position target, int radius) {
    return std::max(std::abs(center.row - target.row),
                    std::abs(center.col - target.col)) <= radius;
}

void testFiveHundredRounds() {
    bool walls[GRID_SIZE][GRID_SIZE];
    mazeWalls(walls);
    bool bombs[GRID_SIZE][GRID_SIZE]{};
    int gold[GRID_SIZE][GRID_SIZE]{};
    Position positions[2] = {{0, 16}, {16, 0}};
    int held[2] = {0, 0};
    int nextRadius = 2;
    int activeActions = 0;
    for (int round = 0; round < 500; ++round) {
        gold[6 + round % 5][6 + (round * 3) % 5] += 1 + round % 5;
        if (round % 13 == 8) gold[2][13] += 80 + round % 41;
        if (round % 17 == 4) gold[16][3] += 15 + round % 9;
        if (round % 20 == 0) {
            for (auto& row : bombs) {
                for (bool& value : row) value = false;
            }
            if (!walls[0][8]) bombs[0][8] = true;
            if (!walls[14][8]) bombs[14][8] = true;
        }
        GameInput input = baseInput(round);
        input.my_units[0] = positions[0];
        input.my_units[1] = positions[1];
        input.my_units_gold[0] = held[0];
        input.my_units_gold[1] = held[1];
        input.gold_opp = round * 3;
        reveal(input, positions[0], nextRadius, walls, gold, bombs);
        reveal(input, positions[1], nextRadius, walls, gold, bombs);
        input.grid[positions[0].row][positions[0].col] =
            gold[positions[0].row][positions[0].col];
        input.grid[positions[1].row][positions[1].col] =
            gold[positions[1].row][positions[1].col];
        Position enemies[2] = {{0, 0}, {16, 16}};
        int enemyCount = 0;
        for (Position enemy : enemies) {
            if ((visibleFrom(positions[0], enemy, nextRadius) ||
                 visibleFrom(positions[1], enemy, nextRadius)) &&
                enemyCount < 2) {
                input.visible_enemies[enemyCount++] = enemy;
            }
        }
        const int npcCount = round % (MAX_NPCS + 1);
        input.num_visible_npcs = npcCount;
        for (int index = 0; index < npcCount; ++index) {
            input.visible_npcs[index].id = index + 1;
            input.visible_npcs[index].pos =
                Position{8 + (index % 3) - 1,
                         8 + ((round + index * 2) % 3) - 1};
        }
        if (round > 0 && round % 5 == 0) fillSnapshot(input);
        const GameOutput output = moveDecision(&input);
        validateOutput(output, round);
        for (int action : output.actions) activeActions += action != kStay;
        const Applied applied = applyOutput(input, output, walls, gold, bombs);
        positions[0] = applied.positions[0];
        positions[1] = applied.positions[1];
        held[0] = applied.held[0];
        held[1] = applied.held[1];
        nextRadius = output.vp == 2 ? 4 : output.vp == 1 ? 3 : 2;
    }
    require(activeActions > 500, "500-round policy remained mostly idle");
    GameInput fresh = baseInput(0);
    const GameOutput reset = moveDecision(&fresh);
    validateOutput(reset, 0);
}

void testDiscontinuities() {
    bool walls[GRID_SIZE][GRID_SIZE]{};
    bool bombs[GRID_SIZE][GRID_SIZE]{};
    int gold[GRID_SIZE][GRID_SIZE]{};
    const int rounds[] = {0, 1, 9, 8, 499, 250, 7, 0};
    for (int round : rounds) {
        GameInput input = baseInput(round);
        input.my_units[0] = Position{1, 1};
        input.my_units[1] = Position{15, 15};
        reveal(input, input.my_units[0], 2, walls, gold, bombs);
        reveal(input, input.my_units[1], 2, walls, gold, bombs);
        const GameOutput output = moveDecision(&input);
        validateOutput(output, round);
    }
}

struct Rng {
    std::uint64_t state = 0x243f6a8885a308d3ULL;

    std::uint32_t next() {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        return static_cast<std::uint32_t>(
            (state * 0x2545f4914f6cdd1dULL) >> 32U);
    }

    int range(int upper) {
        return upper <= 1
                   ? 0
                   : static_cast<int>(next() %
                                      static_cast<std::uint32_t>(upper));
    }
};

Position randomPosition(Rng& rng) {
    return Position{rng.range(GRID_SIZE), rng.range(GRID_SIZE)};
}

void testRandomInputs(std::uint64_t iterations) {
    Rng rng;
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        GameInput input = baseInput(0);
        input.my_units[0] = randomPosition(rng);
        do {
            input.my_units[1] = randomPosition(rng);
        } while (same(input.my_units[0], input.my_units[1]));
        bool walls[GRID_SIZE][GRID_SIZE]{};
        bool bombs[GRID_SIZE][GRID_SIZE]{};
        int gold[GRID_SIZE][GRID_SIZE]{};
        for (int row = 0; row < GRID_SIZE; ++row) {
            for (int col = 0; col < GRID_SIZE; ++col) {
                const int roll = rng.range(100);
                if (roll < 13) {
                    walls[row][col] = true;
                } else if (roll < 17) {
                    bombs[row][col] = true;
                } else if (roll < 43) {
                    gold[row][col] =
                        iteration % 257U == 0U
                            ? std::numeric_limits<int>::max()
                            : 1 + rng.range(1000000);
                }
            }
        }
        for (Position own : input.my_units) {
            walls[own.row][own.col] = false;
            bombs[own.row][own.col] = false;
            gold[own.row][own.col] = 0;
        }
        const int radius = 2 + rng.range(3);
        reveal(input, input.my_units[0], radius, walls, gold, bombs);
        reveal(input, input.my_units[1], radius, walls, gold, bombs);
        int enemyCount = rng.range(3);
        for (int index = 0; index < enemyCount; ++index) {
            Position position = randomPosition(rng);
            if (same(position, input.my_units[0]) ||
                same(position, input.my_units[1]) ||
                (index == 1 && same(position, input.visible_enemies[0]))) {
                continue;
            }
            input.visible_enemies[index] = position;
            walls[position.row][position.col] = false;
            bombs[position.row][position.col] = false;
            input.grid[position.row][position.col] = gold[position.row][position.col];
        }
        input.my_units_gold[0] =
            iteration % 263U == 0U ? std::numeric_limits<int>::max()
                                   : rng.range(100000000);
        input.my_units_gold[1] = rng.range(100000000);
        input.gold_opp = rng.range(100000000);
        input.num_visible_npcs =
            iteration % 271U == 0U ? 100 : rng.range(MAX_NPCS + 1);
        const int fill = input.num_visible_npcs > MAX_NPCS
                             ? MAX_NPCS
                             : input.num_visible_npcs;
        for (int index = 0; index < fill; ++index) {
            input.visible_npcs[index].id = index + 1;
            input.visible_npcs[index].pos = randomPosition(rng);
        }
        if (rng.range(2) != 0) fillSnapshot(input, true);
        if (iteration % 277U == 0U) fillSnapshot(input, false);
        const GameOutput output = moveDecision(&input);
        validateOutput(output, 0);
        (void)applyOutput(input, output, walls, gold, bombs);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t fuzzIterations = 5000;
    if (argc == 3 && std::strcmp(argv[1], "--fuzz") == 0) {
        fuzzIterations = std::strtoull(argv[2], nullptr, 10);
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--fuzz ITERATIONS]\n";
        return 2;
    }
    testFallbacks();
    testFeatureMatrix();
    testFiveHundredRounds();
    testDiscontinuities();
    testRandomInputs(fuzzIterations);
    std::cout << "PASS: " << gChecks << " assertions, " << fuzzIterations
              << " deterministic fuzz inputs\n";
    return 0;
}
