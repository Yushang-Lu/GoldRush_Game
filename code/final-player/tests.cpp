#include "game_api.h"

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
    "..#...#...#...#..", ".##.#.##.##.#.##.",
    "....#.......#....", ".#######.#######.",
    ".......#.#.......", "######.#.#.######",
    ".....#.....#.....", ".###.###.###.###.",
    "...#.........#...", ".#.#.###.###.#.#.",
    ".#.#.#.....#.#.#.", ".#.#.#.#.#.#.#.#.",
    ".#...#.#.#.#...#.", ".###.#.###.#.###.",
    ".................", ".###.###.###.###.",
    "..#...#...#...#.."};

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
    for (NpcInfo& npc : input.visible_npcs) {
        npc.id = 0;
        npc.pos = Position{-1, -1};
    }
    input.snapshot.window_begin = -1;
    input.snapshot.window_end = -1;
    return input;
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

void reveal(GameInput& input, Position center, int radius,
            const bool walls[GRID_SIZE][GRID_SIZE],
            const int gold[GRID_SIZE][GRID_SIZE]) {
    for (int dr = -radius; dr <= radius; ++dr) {
        for (int dc = -radius; dc <= radius; ++dc) {
            const Position position{center.row + dr, center.col + dc};
            if (!inside(position)) continue;
            input.grid[position.row][position.col] =
                walls[position.row][position.col]
                    ? -1
                    : gold[position.row][position.col];
        }
    }
}

void mazeWalls(bool walls[GRID_SIZE][GRID_SIZE]) {
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            walls[row][col] = kMaze[row][col] == '#';
        }
    }
}

void simulate(const GameInput& input, const GameOutput& output,
              const bool walls[GRID_SIZE][GRID_SIZE], Position positions[2],
              bool forbidVisibleBomb = true) {
    positions[0] = input.my_units[0];
    positions[1] = input.my_units[1];
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            if (!inside(next) || walls[next.row][next.col] ||
                same(next, positions[unit ^ 1])) {
                continue;
            }
            bool enemy = false;
            for (Position visible : input.visible_enemies) {
                enemy |= inside(visible) && same(next, visible);
            }
            if (enemy) continue;
            if (forbidVisibleBomb && input.grid[next.row][next.col] == -3) {
                fail("entered a visible bomb", input.round);
            }
            positions[unit] = next;
        }
    }
    require(!same(positions[0], positions[1]), "units ended overlapped",
            input.round);
}

void testFallbacks() {
    GameOutput output = moveDecision(nullptr);
    validateOutput(output, -1);
    require(output.k == 0, "null fallback k");
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

void testFogCornersAndKnownHazards() {
    GameInput input = baseInput(0);
    GameOutput output = moveDecision(&input);
    validateOutput(output, input.round);

    bool walls[GRID_SIZE][GRID_SIZE]{};
    int gold[GRID_SIZE][GRID_SIZE]{};
    input = baseInput(0);
    input.my_units[0] = Position{8, 8};
    input.my_units[1] = Position{12, 12};
    reveal(input, input.my_units[0], 3, walls, gold);
    reveal(input, input.my_units[1], 3, walls, gold);
    input.grid[8][9] = -3;
    input.grid[8][7] = -1;
    walls[8][7] = true;
    input.grid[9][8] = 100;
    input.visible_enemies[0] = Position{8, 10};
    input.num_visible_npcs = 7;
    for (int index = 0; index < MAX_NPCS; ++index) {
        input.visible_npcs[index].id = index + 1;
        input.visible_npcs[index].pos =
            index < 3 ? Position{9, 8} : Position{10, 10};
    }
    input.my_units_gold[0] = 100000;
    output = moveDecision(&input);
    validateOutput(output, input.round);
    Position positions[2];
    simulate(input, output, walls, positions);
}

void fillSnapshot(GameInput& input, int round) {
    input.snapshot_valid = 1;
    input.snapshot.window_begin = round - 5;
    input.snapshot.window_end = round - 1;
    for (int index = 0; index < REGION_COUNT; ++index) {
        input.snapshot.regions[index] = RegionStat{
            index + 1, index, index + 1, 30 + index * 7,
            20 + index * 3, 50 + index * 11, index % 4};
    }
}

void testMazeSequence() {
    bool walls[GRID_SIZE][GRID_SIZE];
    mazeWalls(walls);
    int gold[GRID_SIZE][GRID_SIZE]{};
    Position positions[2] = {{0, 16}, {16, 0}};
    int held[2] = {0, 0};
    int activeMoves = 0;
    for (int round = 0; round < 500; ++round) {
        gold[6 + round % 5][6 + (round * 3) % 5] += 1 + round % 4;
        if (round % 10 == 5) gold[2][13] += 80 + round % 31;
        GameInput input = baseInput(round);
        input.my_units[0] = positions[0];
        input.my_units[1] = positions[1];
        input.my_units_gold[0] = held[0];
        input.my_units_gold[1] = held[1];
        reveal(input, positions[0], 2, walls, gold);
        reveal(input, positions[1], 2, walls, gold);
        if (round % 5 == 0 && round > 0) fillSnapshot(input, round);
        if (round % 37 == 0) {
            input.num_visible_npcs = 3;
            for (int index = 0; index < 3; ++index) {
                input.visible_npcs[index].id = index + 1;
                input.visible_npcs[index].pos = Position{8, 8};
            }
        }
        const GameOutput output = moveDecision(&input);
        validateOutput(output, round);
        require(output.k == 3, "maze branch should use balanced allocation",
                round);
        Position next[2];
        simulate(input, output, walls, next);
        for (int action : output.actions) activeMoves += action != kStay;
        positions[0] = next[0];
        positions[1] = next[1];
        for (int unit = 0; unit < 2; ++unit) {
            const int value = gold[positions[unit].row][positions[unit].col];
            if (value > 0) {
                const int pickup = (value * 65 + 99) / 100;
                gold[positions[unit].row][positions[unit].col] -= pickup;
                held[unit] += pickup;
            }
        }
    }
    require(activeMoves > 2200, "maze search left too many actions idle");

    // A fresh round zero must discard the previous game's map/history.
    GameInput fresh = baseInput(0);
    const GameOutput output = moveDecision(&fresh);
    validateOutput(output, 0);
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
        return upper <= 1
                   ? 0
                   : static_cast<int>(next() % static_cast<unsigned>(upper));
    }
};

Position randomPosition(Rng& rng) {
    return Position{rng.range(GRID_SIZE), rng.range(GRID_SIZE)};
}

void testRandomInputs(std::uint64_t iterations) {
    Rng rng;
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        GameInput input = baseInput(0);  // reset state for independent maps
        input.my_units[0] = randomPosition(rng);
        do {
            input.my_units[1] = randomPosition(rng);
        } while (same(input.my_units[0], input.my_units[1]));
        bool walls[GRID_SIZE][GRID_SIZE]{};
        int gold[GRID_SIZE][GRID_SIZE]{};
        for (Position unit : input.my_units) {
            for (int dr = -3; dr <= 3; ++dr) {
                for (int dc = -3; dc <= 3; ++dc) {
                    const Position position{unit.row + dr, unit.col + dc};
                    if (!inside(position)) continue;
                    const int roll = rng.range(100);
                    if (roll < 9) {
                        walls[position.row][position.col] = true;
                    } else if (roll >= 60) {
                        gold[position.row][position.col] =
                            iteration % 257 == 0
                                ? std::numeric_limits<int>::max()
                                : 1 + rng.range(1000000);
                    }
                }
            }
        }
        walls[input.my_units[0].row][input.my_units[0].col] = false;
        walls[input.my_units[1].row][input.my_units[1].col] = false;
        reveal(input, input.my_units[0], 3, walls, gold);
        reveal(input, input.my_units[1], 3, walls, gold);
        for (Position unit : input.my_units) {
            input.grid[unit.row][unit.col] = 0;
        }
        for (int enemy = 0; enemy < 2; ++enemy) {
            if (rng.range(3) == 0) continue;
            Position position = randomPosition(rng);
            if (same(position, input.my_units[0]) ||
                same(position, input.my_units[1])) {
                continue;
            }
            input.visible_enemies[enemy] = position;
            input.grid[position.row][position.col] = 0;
            walls[position.row][position.col] = false;
        }
        input.my_units_gold[0] =
            iteration % 263 == 0 ? std::numeric_limits<int>::max()
                                 : rng.range(1000000);
        input.my_units_gold[1] = rng.range(1000000);
        input.gold_opp = rng.range(2000000);
        input.num_visible_npcs =
            iteration % 271 == 0 ? 100 : rng.range(MAX_NPCS + 1);
        const int npcFill = input.num_visible_npcs > MAX_NPCS
                                ? MAX_NPCS
                                : input.num_visible_npcs;
        for (int index = 0; index < npcFill; ++index) {
            input.visible_npcs[index].id = index + 1;
            input.visible_npcs[index].pos = randomPosition(rng);
        }
        input.snapshot_valid = rng.range(2);
        if (input.snapshot_valid) fillSnapshot(input, 5);
        if (iteration % 277 == 0) {
            input.snapshot_valid = 1;
            for (RegionStat& stat : input.snapshot.regions) {
                stat.id = 1;  // invalid duplicate IDs: must be ignored safely
                stat.gold_remaining = std::numeric_limits<int>::max();
                stat.occupants = std::numeric_limits<int>::max();
            }
        }
        const GameOutput output = moveDecision(&input);
        validateOutput(output, 0);
        Position positions[2];
        simulate(input, output, walls, positions);
    }
}

void testRoundDiscontinuities() {
    bool walls[GRID_SIZE][GRID_SIZE];
    mazeWalls(walls);
    int gold[GRID_SIZE][GRID_SIZE]{};
    const int rounds[] = {0, 1, 9, 8, 499, 0, 250, 7};
    for (int round : rounds) {
        GameInput input = baseInput(round);
        input.my_units[0] = Position{0, 16};
        input.my_units[1] = Position{16, 0};
        reveal(input, input.my_units[0], 2, walls, gold);
        reveal(input, input.my_units[1], 2, walls, gold);
        const GameOutput output = moveDecision(&input);
        validateOutput(output, round);
        Position positions[2];
        simulate(input, output, walls, positions);
    }
}

void validateSingleBlockSafety(
    const GameInput& input, const GameOutput& output,
    const bool walls[GRID_SIZE][GRID_SIZE]) {
    for (int unit = 0; unit < 2; ++unit) {
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        Position normal = input.my_units[unit];
        Position candidates[S];
        int candidateCount = 0;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{normal.row + kDr[action],
                                normal.col + kDc[action]};
            require(inside(next), "robust route boundary", input.round);
            require(!walls[next.row][next.col], "robust route wall",
                    input.round);
            bool known = false;
            for (int old = 0; old < candidateCount; ++old) {
                known |= same(candidates[old], next);
            }
            if (!known) candidates[candidateCount++] = next;
            normal = next;
        }
        for (int candidate = 0; candidate < candidateCount; ++candidate) {
            Position position = input.my_units[unit];
            for (int index = begin; index < end; ++index) {
                const int action = output.actions[index];
                if (action == kStay) continue;
                const Position next{position.row + kDr[action],
                                    position.col + kDc[action]};
                if (same(next, candidates[candidate])) continue;
                require(inside(next), "blocked-prefix boundary", input.round);
                require(!walls[next.row][next.col], "blocked-prefix wall",
                        input.round);
                require(input.grid[next.row][next.col] != -3,
                        "blocked-prefix bomb", input.round);
                position = next;
            }
        }
    }
}

void testObservedBlockFallback() {
    bool walls[GRID_SIZE][GRID_SIZE];
    mazeWalls(walls);
    int gold[GRID_SIZE][GRID_SIZE]{};
    GameInput first = baseInput(0);
    first.my_units[0] = Position{0, 16};
    first.my_units[1] = Position{16, 0};
    reveal(first, first.my_units[0], 2, walls, gold);
    reveal(first, first.my_units[1], 2, walls, gold);
    const GameOutput planned = moveDecision(&first);
    validateOutput(planned, 0);

    // Keep both units at their old positions: this cannot match a non-trivial
    // prediction and emulates an earlier opponent occupying a narrow exit.
    GameInput blocked = first;
    blocked.round = 1;
    const GameOutput conservative = moveDecision(&blocked);
    validateOutput(conservative, 1);
    validateSingleBlockSafety(blocked, conservative, walls);
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
    testFallbacks();
    testFogCornersAndKnownHazards();
    testMazeSequence();
    testRandomInputs(fuzzIterations);
    testRoundDiscontinuities();
    testObservedBlockFallback();
    std::cout << "PASS: " << gChecks << " assertions, " << fuzzIterations
              << " deterministic fuzz inputs\n";
    return 0;
}
