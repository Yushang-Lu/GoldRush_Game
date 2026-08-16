#include "strategy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

#ifndef SLOW_PLAYER_THINK_US
#define SLOW_PLAYER_THINK_US 240000
#endif

#ifndef SLOW_PLAYER_EXHAUSTIVE
#define SLOW_PLAYER_EXHAUSTIVE 1
#endif

namespace slow_player {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kCells = GRID_SIZE * GRID_SIZE;
constexpr int kStay = 4;
constexpr int kInf = 30000;
constexpr std::uint16_t kDistInf = 0xffffU;
constexpr int kTopPlans = SLOW_PLAYER_EXHAUSTIVE ? 384 : 96;
constexpr int kScenarioCount = SLOW_PLAYER_EXHAUSTIVE ? 96 : 8;
constexpr int kFinalists = SLOW_PLAYER_EXHAUSTIVE ? 36 : 12;
constexpr int kThinkUs = SLOW_PLAYER_THINK_US;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

// The four public-log boards are priors, never unquestioned truth.  '#' is a
// wall, 'H' is a high-value outer spawn marker found in the log header, and
// '.' is ordinary open ground.  Every observed cell is checked against every
// still-possible template; a contradiction removes the template immediately.
constexpr const char* kPublicMaps[4][GRID_SIZE] = {
    {"...#HH......H#...", "......H...H......", ".##...........##.",
     "#..#.........#..#", "....#.......#....", "H....#.....#....H",
     "...#.........#...", ".H.....#.#.....H.", "....#.#...#.#....",
     "..H....#.#....H..", "...#.........#...", "H....#.....#....H",
     "...H#.......#H...", "#..#.........#..#", ".##...........##.",
     "......H...H......", "...#HH......H#..."},
    {"....H...H........", "......H.....H....", "..#...#...#...#..",
     "..........H......", "....#...#...#....", "H...............H",
     "..#...#...#...#..", "..H...........H..", "....#.......#....",
     ".H.............H.", "..#...#...#...#..", "...H.........H...",
     "H...#...#...#...H", "..........H......", "..#...#...#...#..",
     "......H.....H....", "....H...H........"},
    {"......HHHHH......", ".................", "..###.......###..",
     "..###.......###..", "....####.####....", "....####.####....",
     "H...####.####...H", "H...............H", "H...###...###...H",
     "H...............H", "H...####.####...H", "....####.####....",
     "....####.####....", "..###.......###..", "..###.......###..",
     ".................", "......HHHHH......"},
    {"..#...#.H.#...#..", ".##.#.##.##.#.##.", "...H#.......#H...",
     ".#######.#######.", ".......#.#.......", "######.#.#.######",
     ".....#.....#.....", ".###.###.###.###.", "...#.........#...",
     ".#.#.###.###.#.#.", ".#.#.#.....#.#.#.", ".#.#.#.#.#.#.#.#.",
     ".#...#.#.#.#...#.", ".###.#.###.#.###.", ".................",
     ".###.###.###.###.", "..#H..#.H.#..H#.."}};

enum Terrain : std::int8_t {
    kTerrainUnknown = -1,
    kTerrainOpen = 0,
    kTerrainWall = 1,
};

struct Rng {
    std::uint64_t state;

    explicit Rng(std::uint64_t seed) noexcept
        : state(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

    std::uint64_t next64() noexcept {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    std::uint32_t next() noexcept {
        return static_cast<std::uint32_t>(next64() >> 32U);
    }

    int range(int upper) noexcept {
        return upper <= 1
                   ? 0
                   : static_cast<int>(next() %
                                      static_cast<std::uint32_t>(upper));
    }

    double unit() noexcept {
        return static_cast<double>(next() >> 8U) * (1.0 / 16777216.0);
    }
};

inline bool inside(int row, int col) noexcept {
    return static_cast<unsigned>(row) < static_cast<unsigned>(GRID_SIZE) &&
           static_cast<unsigned>(col) < static_cast<unsigned>(GRID_SIZE);
}

inline bool inside(Position position) noexcept {
    return inside(position.row, position.col);
}

inline bool same(Position first, Position second) noexcept {
    return first.row == second.row && first.col == second.col;
}

inline int cellOf(int row, int col) noexcept {
    return row * GRID_SIZE + col;
}

inline int cellOf(Position position) noexcept {
    return inside(position) ? cellOf(position.row, position.col) : -1;
}

inline Position positionOf(int cell) noexcept {
    return Position{cell / GRID_SIZE, cell % GRID_SIZE};
}

inline int absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

inline int manhattan(int first, int second) noexcept {
    const Position a = positionOf(first);
    const Position b = positionOf(second);
    return absInt(a.row - b.row) + absInt(a.col - b.col);
}

inline int regionOf(int cell) noexcept {
    const Position p = positionOf(cell);
    if (p.row >= 4 && p.row <= 12 && p.col >= 4 && p.col <= 12) return 1;
    if (p.row <= 3 && p.col <= 12) return 2;
    if (p.row >= 4 && p.col <= 3) return 3;
    if (p.row >= 13 && p.col >= 4) return 4;
    return 5;
}

inline int clampCount(int value, int upper) noexcept {
    if (value <= 0) return 0;
    return value > upper ? upper : value;
}

inline int clampGold(int value) noexcept {
    return clampCount(value, 100000000);
}

inline double clampDouble(double value, double low, double high) noexcept {
    return value < low ? low : value > high ? high : value;
}

inline int pickup65(int remaining) noexcept {
    if (remaining <= 0) return 0;
    const std::int64_t value = remaining;
    return static_cast<int>((value * 65 + 99) / 100);
}

inline int loss10(int held) noexcept {
    return held <= 0 ? 0 : (held + 9) / 10;
}

inline int loss5(int held) noexcept {
    return held <= 0 ? 0 : (held + 19) / 20;
}

GameOutput fallback() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

bool validInput(const GameInput* input) noexcept {
    return input != nullptr && input->round >= 0 && input->round <= 1000000 &&
           inside(input->my_units[0]) && inside(input->my_units[1]) &&
           !same(input->my_units[0], input->my_units[1]);
}

struct CellMemory {
    Terrain terrain;
    int lastSeen;
    int lastValue;
    int lastVisit;
    int lastPositive;
    double goldEvidence;
    double spawnEvidence;
    double blockEvidence;
};

struct RegionBelief {
    bool valid;
    int observedRound;
    RegionStat latest;
    double generationEwma;
    double collectedEwma;
    double remainingEwma;
};

struct ActorBelief {
    int id;
    bool assigned;
    int lastSeen;
    int lastCell;
    double probability[kCells];
    double direction[5];
};

struct PreviousPlan {
    bool valid;
    int round;
    Position starts[2];
    GameOutput output;
};

struct Memory {
    bool initialized;
    int lastRound;
    std::uint64_t gameSerial;
    unsigned templateMask;
    int templateSeen;
    CellMemory cells[kCells];
    RegionBelief regions[REGION_COUNT];
    ActorBelief enemies[2];
    ActorBelief npcs[MAX_NPCS];
    PreviousPlan previous;
    int visionSpent;
    int lastOuterEvent;
    double outerEventAmount;
    int outerIntervals[16];
    int outerIntervalCount;
};

Memory gMemory{};

bool recognizedGridValue(int value) noexcept {
    return value == -5 || value == -3 || value == -1 || value >= 0;
}

bool templateWall(int map, int cell) noexcept {
    const Position p = positionOf(cell);
    return kPublicMaps[map][p.row][p.col] == '#';
}

bool templateHotspot(int map, int cell) noexcept {
    const Position p = positionOf(cell);
    return kPublicMaps[map][p.row][p.col] == 'H';
}

int bitCount(unsigned value) noexcept {
    int result = 0;
    while (value != 0U) {
        result += static_cast<int>(value & 1U);
        value >>= 1U;
    }
    return result;
}

int firstTemplate(unsigned mask) noexcept {
    for (int map = 0; map < 4; ++map) {
        if ((mask & (1U << static_cast<unsigned>(map))) != 0U) return map;
    }
    return -1;
}

void clearActor(ActorBelief& actor, int id = 0) noexcept {
    actor.id = id;
    actor.assigned = false;
    actor.lastSeen = -1000000;
    actor.lastCell = -1;
    for (double& value : actor.probability) value = 0.0;
    for (double& value : actor.direction) value = 1.0;
}

void setActorCell(ActorBelief& actor, int cell, int round) noexcept {
    for (double& value : actor.probability) value = 0.0;
    if (cell >= 0 && cell < kCells) actor.probability[cell] = 1.0;
    actor.assigned = true;
    actor.lastSeen = round;
    actor.lastCell = cell;
}

void resetMemory(const GameInput& input) noexcept {
    gMemory.initialized = true;
    gMemory.lastRound = input.round;
    ++gMemory.gameSerial;
    gMemory.templateMask = 0x0fU;
    gMemory.templateSeen = 0;
    gMemory.previous.valid = false;
    gMemory.visionSpent = 0;
    gMemory.lastOuterEvent = -1000000;
    gMemory.outerEventAmount = 120.0;
    gMemory.outerIntervalCount = 0;
    for (int& interval : gMemory.outerIntervals) interval = 0;
    for (CellMemory& cell : gMemory.cells) {
        cell.terrain = kTerrainUnknown;
        cell.lastSeen = -1000000;
        cell.lastValue = -5;
        cell.lastVisit = -1000000;
        cell.lastPositive = -1000000;
        cell.goldEvidence = 0.0;
        cell.spawnEvidence = 0.0;
        cell.blockEvidence = 0.0;
    }
    for (RegionBelief& region : gMemory.regions) {
        region.valid = false;
        region.observedRound = -1000000;
        region.latest = RegionStat{};
        region.generationEwma = 0.0;
        region.collectedEwma = 0.0;
        region.remainingEwma = 0.0;
    }
    clearActor(gMemory.enemies[0], -101);
    clearActor(gMemory.enemies[1], -102);
    for (int index = 0; index < MAX_NPCS; ++index) {
        clearActor(gMemory.npcs[index]);
        if (input.round == 0) {
            setActorCell(gMemory.npcs[index], cellOf(8, 8), input.round);
        } else {
            gMemory.npcs[index].assigned = true;
            const double uniform = 1.0 / static_cast<double>(kCells);
            for (double& value : gMemory.npcs[index].probability) {
                value = uniform;
            }
        }
    }

    // The two players occupy opposite diagonals.  This is only used when a
    // normal round-zero input actually has the two expected corner starts.
    const int first = cellOf(input.my_units[0]);
    const int second = cellOf(input.my_units[1]);
    const bool mainDiagonal =
        (first == cellOf(0, 0) && second == cellOf(16, 16)) ||
        (second == cellOf(0, 0) && first == cellOf(16, 16));
    const bool antiDiagonal =
        (first == cellOf(0, 16) && second == cellOf(16, 0)) ||
        (second == cellOf(0, 16) && first == cellOf(16, 0));
    if (mainDiagonal) {
        setActorCell(gMemory.enemies[0], cellOf(0, 16), input.round);
        setActorCell(gMemory.enemies[1], cellOf(16, 0), input.round);
    } else if (antiDiagonal) {
        setActorCell(gMemory.enemies[0], cellOf(0, 0), input.round);
        setActorCell(gMemory.enemies[1], cellOf(16, 16), input.round);
    }
}

int consensusTerrain(int cell) noexcept {
    const Terrain observed = gMemory.cells[cell].terrain;
    if (observed == kTerrainWall) return 1;
    if (observed == kTerrainOpen) return 0;
    if (gMemory.templateMask == 0U) return -1;
    bool anyWall = false;
    bool anyOpen = false;
    for (int map = 0; map < 4; ++map) {
        if ((gMemory.templateMask & (1U << static_cast<unsigned>(map))) == 0U) {
            continue;
        }
        if (templateWall(map, cell)) {
            anyWall = true;
        } else {
            anyOpen = true;
        }
    }
    return anyWall && anyOpen ? -1 : anyWall ? 1 : 0;
}

void updateTemplateMask(const GameInput& input) noexcept {
    unsigned mask = gMemory.templateMask;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value == -5 || !recognizedGridValue(value)) continue;
            ++gMemory.templateSeen;
            const bool wall = value == -1;
            const int cell = cellOf(row, col);
            for (int map = 0; map < 4; ++map) {
                const unsigned bit = 1U << static_cast<unsigned>(map);
                if ((mask & bit) != 0U && templateWall(map, cell) != wall) {
                    mask &= ~bit;
                }
            }
        }
    }
    gMemory.templateMask = mask;
}

void updateCells(const GameInput& input) noexcept {
    updateTemplateMask(input);
    for (CellMemory& cell : gMemory.cells) {
        cell.blockEvidence *= 0.82;
        cell.goldEvidence *= 0.997;
        cell.spawnEvidence *= 0.999;
    }
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value == -5 || !recognizedGridValue(value)) continue;
            const int cellIndex = cellOf(row, col);
            CellMemory& cell = gMemory.cells[cellIndex];
            const int oldValue = cell.lastValue;
            const int oldSeen = cell.lastSeen;
            cell.terrain = value == -1 ? kTerrainWall : kTerrainOpen;
            if (value > 0) {
                const int bounded = clampGold(value);
                cell.lastPositive = input.round;
                cell.goldEvidence = std::min(
                    1000000.0, cell.goldEvidence * 0.92 + bounded);
                if (oldSeen + 1 != input.round || oldValue <= 0 ||
                    value > oldValue) {
                    const int delta = oldValue > 0 && value > oldValue
                                          ? value - oldValue
                                          : value;
                    cell.spawnEvidence = std::min(
                        1000000.0, cell.spawnEvidence * 0.95 +
                                       static_cast<double>(clampGold(delta)));
                }
            }
            cell.lastSeen = input.round;
            cell.lastValue = value;
        }
    }
    for (Position unit : input.my_units) {
        const int cell = cellOf(unit);
        gMemory.cells[cell].terrain = kTerrainOpen;
        gMemory.cells[cell].lastVisit = input.round;
    }
}

bool validSnapshot(const GameInput& input) noexcept {
    if (input.snapshot_valid != 1) return false;
    bool used[REGION_COUNT] = {false, false, false, false, false};
    for (const RegionStat& region : input.snapshot.regions) {
        if (region.id < 1 || region.id > REGION_COUNT || used[region.id - 1]) {
            return false;
        }
        used[region.id - 1] = true;
    }
    return true;
}

void updateSnapshot(const GameInput& input) noexcept {
    if (!validSnapshot(input)) return;
    int outerGenerated = 0;
    for (const RegionStat& raw : input.snapshot.regions) {
        const int index = raw.id - 1;
        RegionBelief& belief = gMemory.regions[index];
        RegionStat clean = raw;
        clean.enter = clampCount(clean.enter, 1000000);
        clean.leave = clampCount(clean.leave, 1000000);
        clean.gold_generated = clampCount(clean.gold_generated, 100000000);
        clean.gold_collected = clampCount(clean.gold_collected, 100000000);
        clean.gold_remaining = clampCount(clean.gold_remaining, 100000000);
        clean.occupants = clampCount(clean.occupants, 1000);
        const double alpha = belief.valid ? 0.22 : 1.0;
        belief.generationEwma +=
            alpha * (static_cast<double>(clean.gold_generated) -
                     belief.generationEwma);
        belief.collectedEwma +=
            alpha * (static_cast<double>(clean.gold_collected) -
                     belief.collectedEwma);
        belief.remainingEwma +=
            alpha * (static_cast<double>(clean.gold_remaining) -
                     belief.remainingEwma);
        belief.valid = true;
        belief.observedRound = input.round;
        belief.latest = clean;
        if (clean.id != 1) outerGenerated += clean.gold_generated;
    }
    if (outerGenerated >= 70) {
        if (gMemory.lastOuterEvent > -100000 &&
            input.round > gMemory.lastOuterEvent) {
            const int interval = input.round - gMemory.lastOuterEvent;
            if (gMemory.outerIntervalCount < 16) {
                gMemory.outerIntervals[gMemory.outerIntervalCount++] = interval;
            } else {
                for (int index = 1; index < 16; ++index) {
                    gMemory.outerIntervals[index - 1] =
                        gMemory.outerIntervals[index];
                }
                gMemory.outerIntervals[15] = interval;
            }
        }
        gMemory.lastOuterEvent = input.round;
        gMemory.outerEventAmount =
            0.78 * gMemory.outerEventAmount + 0.22 * outerGenerated;
    }
}

bool hardWallKnown(int cell) noexcept {
    return cell < 0 || cell >= kCells || consensusTerrain(cell) == 1;
}

void propagateActor(ActorBelief& actor, int steps) noexcept {
    if (!actor.assigned || steps <= 0) return;
    double next[kCells];
    for (int step = 0; step < steps; ++step) {
        for (double& value : next) value = 0.0;
        for (int cell = 0; cell < kCells; ++cell) {
            const double mass = actor.probability[cell];
            if (mass <= 1e-15) continue;
            const Position p = positionOf(cell);
            int options[5];
            int count = 0;
            options[count++] = cell;
            for (int action = 0; action < 4; ++action) {
                const int row = p.row + kDr[action];
                const int col = p.col + kDc[action];
                if (!inside(row, col)) continue;
                const int target = cellOf(row, col);
                if (!hardWallKnown(target)) options[count++] = target;
            }
            const double share = mass / static_cast<double>(count);
            for (int index = 0; index < count; ++index) {
                next[options[index]] += share;
            }
        }
        std::memcpy(actor.probability, next, sizeof(next));
    }
}

int actorMode(const ActorBelief& actor) noexcept {
    int best = actor.lastCell;
    double value = -1.0;
    for (int cell = 0; cell < kCells; ++cell) {
        if (actor.probability[cell] > value) {
            value = actor.probability[cell];
            best = cell;
        }
    }
    return best;
}

void observeActor(ActorBelief& actor, int id, int cell, int round) noexcept {
    if (actor.assigned && actor.lastSeen + 1 == round && actor.lastCell >= 0) {
        const Position old = positionOf(actor.lastCell);
        const Position now = positionOf(cell);
        const int dr = now.row - old.row;
        const int dc = now.col - old.col;
        int direction = kStay;
        if (absInt(dr) >= absInt(dc) && dr != 0) {
            direction = dr < 0 ? 0 : 1;
        } else if (dc != 0) {
            direction = dc < 0 ? 2 : 3;
        }
        actor.direction[direction] += 1.0;
    }
    actor.id = id;
    setActorCell(actor, cell, round);
}

void updateEnemies(const GameInput& input) noexcept {
    int visible[2] = {-1, -1};
    int count = 0;
    for (Position position : input.visible_enemies) {
        const int cell = cellOf(position);
        if (cell >= 0 && count < 2) visible[count++] = cell;
    }
    bool observed[2] = {false, false};
    if (count == 2) {
        const int mode0 = actorMode(gMemory.enemies[0]);
        const int mode1 = actorMode(gMemory.enemies[1]);
        const int direct = manhattan(mode0, visible[0]) +
                           manhattan(mode1, visible[1]);
        const int swapped = manhattan(mode0, visible[1]) +
                            manhattan(mode1, visible[0]);
        const int first = swapped < direct ? 1 : 0;
        observeActor(gMemory.enemies[0], -101, visible[first], input.round);
        observeActor(gMemory.enemies[1], -102, visible[first ^ 1], input.round);
        observed[0] = observed[1] = true;
    } else if (count == 1) {
        const int mode0 = actorMode(gMemory.enemies[0]);
        const int mode1 = actorMode(gMemory.enemies[1]);
        const int chosen = !gMemory.enemies[0].assigned
                               ? 0
                               : !gMemory.enemies[1].assigned
                                     ? 1
                                     : manhattan(mode0, visible[0]) <=
                                               manhattan(mode1, visible[0])
                                           ? 0
                                           : 1;
        observeActor(gMemory.enemies[chosen], chosen == 0 ? -101 : -102,
                     visible[0], input.round);
        observed[chosen] = true;
    }
    for (int index = 0; index < 2; ++index) {
        if (!observed[index] &&
            gMemory.enemies[index].lastSeen < input.round) {
            propagateActor(gMemory.enemies[index], 6);
        }
    }
}

int findNpcSlot(int id) noexcept {
    for (int index = 0; index < MAX_NPCS; ++index) {
        if (gMemory.npcs[index].assigned && gMemory.npcs[index].id == id) {
            return index;
        }
    }
    for (int index = 0; index < MAX_NPCS; ++index) {
        if (gMemory.npcs[index].id == 0) return index;
    }
    int oldest = 0;
    for (int index = 1; index < MAX_NPCS; ++index) {
        if (gMemory.npcs[index].lastSeen < gMemory.npcs[oldest].lastSeen) {
            oldest = index;
        }
    }
    return oldest;
}

void updateNpcs(const GameInput& input) noexcept {
    bool observed[MAX_NPCS] = {false, false, false, false, false, false, false};
    const int count = clampCount(input.num_visible_npcs, MAX_NPCS);
    for (int index = 0; index < count; ++index) {
        const NpcInfo& info = input.visible_npcs[index];
        const int cell = cellOf(info.pos);
        if (info.id == 0 || cell < 0) continue;
        const int slot = findNpcSlot(info.id);
        observeActor(gMemory.npcs[slot], info.id, cell, input.round);
        observed[slot] = true;
    }
    for (int index = 0; index < MAX_NPCS; ++index) {
        if (!observed[index] && gMemory.npcs[index].lastSeen < input.round) {
            propagateActor(gMemory.npcs[index], 3);
        }
    }
}

struct ReplayResult {
    Position positions[2];
    int forcedCells[S];
};

ReplayResult replayPrevious(unsigned blockedMask) noexcept {
    ReplayResult result{{gMemory.previous.starts[0],
                         gMemory.previous.starts[1]},
                        {-1, -1, -1, -1, -1, -1}};
    const GameOutput& output = gMemory.previous.output;
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{result.positions[unit].row + kDr[action],
                                result.positions[unit].col + kDc[action]};
            const int target = cellOf(next);
            const bool forced =
                (blockedMask & (1U << static_cast<unsigned>(index))) != 0U;
            if (forced) result.forcedCells[index] = target;
            if (forced || target < 0 || hardWallKnown(target) ||
                same(next, result.positions[unit ^ 1])) {
                continue;
            }
            result.positions[unit] = next;
        }
    }
    return result;
}

void inferPreviousBlocks(const GameInput& input) noexcept {
    if (!gMemory.previous.valid ||
        gMemory.previous.round + 1 != input.round) {
        return;
    }
    int minimum = S + 1;
    bool matches[1U << S]{};
    for (unsigned mask = 0; mask < (1U << S); ++mask) {
        const ReplayResult replay = replayPrevious(mask);
        if (!same(replay.positions[0], input.my_units[0]) ||
            !same(replay.positions[1], input.my_units[1])) {
            continue;
        }
        const int blocked = bitCount(mask);
        if (blocked < minimum) {
            minimum = blocked;
            for (bool& value : matches) value = false;
        }
        if (blocked == minimum) matches[mask] = true;
    }
    if (minimum > S) return;
    double evidence[kCells]{};
    int matchCount = 0;
    for (unsigned mask = 0; mask < (1U << S); ++mask) {
        if (!matches[mask]) continue;
        ++matchCount;
        const ReplayResult replay = replayPrevious(mask);
        for (int cell : replay.forcedCells) {
            if (cell >= 0 && cell < kCells) evidence[cell] += 1.0;
        }
    }
    if (matchCount <= 0) return;
    for (int cell = 0; cell < kCells; ++cell) {
        gMemory.cells[cell].blockEvidence +=
            2.0 * evidence[cell] / static_cast<double>(matchCount);
    }
}

void updateMemory(const GameInput& input) noexcept {
    inferPreviousBlocks(input);
    updateCells(input);
    updateSnapshot(input);
    updateEnemies(input);
    updateNpcs(input);
    gMemory.lastRound = input.round;
}

std::uint64_t hashInput(const GameInput& input) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        hash *= 0x100000001b3ULL;
    };
    mix(static_cast<std::uint64_t>(input.round));
    mix(static_cast<std::uint64_t>(cellOf(input.my_units[0]) + 1));
    mix(static_cast<std::uint64_t>(cellOf(input.my_units[1]) + 1));
    mix(static_cast<std::uint64_t>(clampGold(input.my_units_gold[0])));
    mix(static_cast<std::uint64_t>(clampGold(input.my_units_gold[1])));
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value != -5) {
                const std::int64_t encoded =
                    static_cast<std::int64_t>(cellOf(row, col)) * 131 +
                    static_cast<std::int64_t>(value) + 7;
                mix(static_cast<std::uint64_t>(encoded));
            }
        }
    }
    return hash;
}

struct Model {
    bool wall[kCells];
    bool uncertain[kCells];
    bool hotspot[kCells];
    double hotspotProbability[kCells];
    double gold[kCells];
    double survival[kCells];
    double bomb[kCells];
    double npcTrample[kCells];
    double enemyBlock[kCells];
    double information[kCells];
    double terminal[kCells];
    int bestGoal[kCells];
    int openByRegion[REGION_COUNT];
    std::uint16_t distance[kCells][kCells];
    double observedWallRate;
    double outerHazard;
};

void buildDistances(Model& model) noexcept {
    int queue[kCells];
    for (int source = 0; source < kCells; ++source) {
        for (int cell = 0; cell < kCells; ++cell) {
            model.distance[source][cell] = kDistInf;
        }
        if (model.wall[source]) continue;
        int begin = 0;
        int end = 0;
        queue[end++] = source;
        model.distance[source][source] = 0;
        while (begin < end) {
            const int cell = queue[begin++];
            const Position p = positionOf(cell);
            const std::uint16_t nextDistance = static_cast<std::uint16_t>(
                model.distance[source][cell] + 1U);
            for (int action = 0; action < 4; ++action) {
                const int row = p.row + kDr[action];
                const int col = p.col + kDc[action];
                if (!inside(row, col)) continue;
                const int target = cellOf(row, col);
                if (model.wall[target] ||
                    model.distance[source][target] != kDistInf) {
                    continue;
                }
                model.distance[source][target] = nextDistance;
                queue[end++] = target;
            }
        }
    }
}

double actorReach(const ActorBelief& actor, int target, int steps,
                  const Model& model) noexcept {
    if (!actor.assigned) return 0.0;
    double probability = 0.0;
    for (int cell = 0; cell < kCells; ++cell) {
        const double mass = actor.probability[cell];
        if (mass <= 1e-12) continue;
        const std::uint16_t distance = model.distance[cell][target];
        if (distance == kDistInf || distance > steps) continue;
        const double reach =
            1.0 - static_cast<double>(distance) /
                      static_cast<double>(steps + 2);
        probability += mass * reach;
    }
    return clampDouble(probability, 0.0, 1.0);
}

double outerHazard(int round) noexcept {
    const int age = round - gMemory.lastOuterEvent;
    if (gMemory.lastOuterEvent < -100000) return 0.075;
    double mean = 13.0;
    if (gMemory.outerIntervalCount > 0) {
        mean = 0.0;
        for (int index = 0; index < gMemory.outerIntervalCount; ++index) {
            mean += gMemory.outerIntervals[index];
        }
        mean /= gMemory.outerIntervalCount;
    }
    if (age < 5) return 0.015;
    if (age < mean - 3.0) return 0.05;
    if (age < mean + 2.0) return 0.11;
    return 0.22;
}

void buildModel(const GameInput& input, Model& model) noexcept {
    int knownWalls = 0;
    int knownCells = 0;
    for (int& count : model.openByRegion) count = 0;
    const int exactMap = bitCount(gMemory.templateMask) == 1
                             ? firstTemplate(gMemory.templateMask)
                             : -1;
    for (int cell = 0; cell < kCells; ++cell) {
        const int consensus = consensusTerrain(cell);
        model.wall[cell] = consensus == 1;
        model.uncertain[cell] = consensus < 0;
        if (consensus >= 0) {
            ++knownCells;
            knownWalls += consensus == 1;
        }
        model.hotspot[cell] = exactMap >= 0 && templateHotspot(exactMap, cell);
        double hotspotProbability = 0.0;
        int candidates = 0;
        for (int map = 0; map < 4; ++map) {
            if ((gMemory.templateMask & (1U << static_cast<unsigned>(map))) ==
                0U) {
                continue;
            }
            ++candidates;
            hotspotProbability += templateHotspot(map, cell) ? 1.0 : 0.0;
        }
        model.hotspotProbability[cell] =
            candidates > 0 ? hotspotProbability / candidates : 0.0;
        if (!model.wall[cell]) ++model.openByRegion[regionOf(cell) - 1];
    }
    for (Position own : input.my_units) {
        const int cell = cellOf(own);
        model.wall[cell] = false;
        model.uncertain[cell] = false;
    }
    model.observedWallRate = knownCells > 0
                                 ? clampDouble(static_cast<double>(knownWalls) /
                                                   knownCells,
                                               0.04, 0.42)
                                 : 0.16;
    model.outerHazard = outerHazard(input.round);
    buildDistances(model);

    double regionResidual[REGION_COUNT]{};
    double regionPressure[REGION_COUNT]{};
    for (int region = 0; region < REGION_COUNT; ++region) {
        const RegionBelief& belief = gMemory.regions[region];
        if (!belief.valid) {
            regionResidual[region] = region == 0 ? 18.0 : 8.0;
            regionPressure[region] = region == 0 ? 0.35 : 0.18;
            continue;
        }
        const int age = std::max(0, input.round - belief.observedRound);
        const double collectionRatio =
            static_cast<double>(belief.latest.gold_collected + 1) /
            static_cast<double>(belief.latest.gold_generated + 8);
        const double decay = std::exp(
            -0.055 * age * (1.0 + clampDouble(collectionRatio, 0.0, 2.0)));
        regionResidual[region] =
            clampDouble(belief.latest.gold_remaining * decay, 0.0, 1000000.0);
        regionPressure[region] = clampDouble(
            belief.latest.occupants / 9.0 + collectionRatio * 0.35, 0.0, 2.5);
    }

    int hotspotByRegion[REGION_COUNT]{};
    for (int cell = 0; cell < kCells; ++cell) {
        if (!model.wall[cell] && model.hotspotProbability[cell] > 0.45) {
            ++hotspotByRegion[regionOf(cell) - 1];
        }
    }

    for (int cell = 0; cell < kCells; ++cell) {
        model.gold[cell] = 0.0;
        model.survival[cell] = 1.0;
        model.bomb[cell] = 0.0;
        model.npcTrample[cell] = 0.0;
        model.enemyBlock[cell] = 0.0;
        model.information[cell] = 0.0;
        model.terminal[cell] = 0.0;
        model.bestGoal[cell] = cell;
        if (model.wall[cell]) continue;
        const Position p = positionOf(cell);
        const int value = input.grid[p.row][p.col];
        const CellMemory& memory = gMemory.cells[cell];
        const int region = regionOf(cell) - 1;
        const bool currentlyVisible = value != -5 && recognizedGridValue(value);
        if (currentlyVisible) {
            model.gold[cell] = value > 0 ? clampGold(value) : 0.0;
        } else {
            if (memory.lastValue > 0) {
                const int age = std::max(0, input.round - memory.lastSeen);
                const double decay =
                    std::exp(-0.10 * age * (1.0 + regionPressure[region]));
                model.gold[cell] += clampGold(memory.lastValue) * decay;
            }
            const int open = std::max(1, model.openByRegion[region]);
            double residualShare = regionResidual[region] / open;
            if (region != 0 && model.hotspotProbability[cell] > 0.0) {
                const int hotspots = std::max(1, hotspotByRegion[region]);
                residualShare =
                    0.32 * regionResidual[region] / open +
                    0.68 * regionResidual[region] /
                        static_cast<double>(hotspots) *
                        model.hotspotProbability[cell];
            }
            model.gold[cell] += residualShare;
            model.gold[cell] += std::min(35.0, memory.goldEvidence * 0.025);
            model.gold[cell] += std::min(25.0, memory.spawnEvidence * 0.035);
            if (region != 0 && model.hotspotProbability[cell] > 0.0) {
                int totalHotspots = 0;
                for (int other = 0; other < kCells; ++other) {
                    totalHotspots +=
                        !model.wall[other] &&
                        model.hotspotProbability[other] > 0.45;
                }
                model.gold[cell] +=
                    model.outerHazard * gMemory.outerEventAmount *
                    model.hotspotProbability[cell] /
                    std::max(1, totalHotspots);
            }
        }
        model.gold[cell] = clampDouble(model.gold[cell], 0.0, 100000000.0);

        if (value == -3) {
            model.bomb[cell] = 1.0;
        } else if (memory.lastValue == -3 &&
                   memory.lastSeen / 20 == input.round / 20) {
            const int age = std::max(0, input.round - memory.lastSeen);
            model.bomb[cell] = std::exp(-0.22 * age);
        }

        double steal = 0.0;
        for (const ActorBelief& enemy : gMemory.enemies) {
            const double reach = actorReach(enemy, cell, 6, model) * 0.78;
            steal = 1.0 - (1.0 - steal) * (1.0 - reach);
            model.enemyBlock[cell] += actorReach(enemy, cell, 6, model) * 0.16;
        }
        double crowdMean = 0.0;
        for (const ActorBelief& npc : gMemory.npcs) {
            const double reach = actorReach(npc, cell, 3, model);
            crowdMean += reach;
            steal = 1.0 - (1.0 - steal) * (1.0 - reach * 0.48);
        }
        model.survival[cell] = clampDouble(1.0 - steal, 0.05, 1.0);
        if (crowdMean >= 3.0) {
            model.npcTrample[cell] = clampDouble((crowdMean - 2.0) / 2.5,
                                                 0.0, 0.92);
        } else {
            const double lambda = crowdMean;
            const double p0 = std::exp(-lambda);
            const double p1 = p0 * lambda;
            const double p2 = p1 * lambda * 0.5;
            model.npcTrample[cell] =
                clampDouble(1.0 - p0 - p1 - p2, 0.0, 0.75);
        }
        model.enemyBlock[cell] = clampDouble(
            model.enemyBlock[cell] + memory.blockEvidence * 0.12, 0.0, 0.85);
    }

    for (int endpoint = 0; endpoint < kCells; ++endpoint) {
        if (model.wall[endpoint]) continue;
        const Position center = positionOf(endpoint);
        double information = 0.0;
        for (int dr = -2; dr <= 2; ++dr) {
            for (int dc = -2; dc <= 2; ++dc) {
                const int row = center.row + dr;
                const int col = center.col + dc;
                if (!inside(row, col)) continue;
                const CellMemory& memory = gMemory.cells[cellOf(row, col)];
                if (memory.terrain == kTerrainUnknown) information += 0.22;
                const int age = input.round - memory.lastSeen;
                if (age > 0) information += std::min(0.20, age * 0.008);
            }
        }
        model.information[endpoint] = information;
        double best = 0.0;
        int goal = endpoint;
        for (int target = 0; target < kCells; ++target) {
            if (model.wall[target] || model.gold[target] <= 0.02) continue;
            const std::uint16_t rawDistance = model.distance[endpoint][target];
            if (rawDistance == kDistInf) continue;
            const double distance = rawDistance;
            double utility = model.gold[target] * model.survival[target] /
                             (1.0 + 0.19 * distance);
            utility -= 0.11 * distance;
            utility -= model.bomb[target] * 2.0;
            if (model.hotspotProbability[target] > 0.45) utility += 0.35;
            if (utility > best) {
                best = utility;
                goal = target;
            }
        }
        model.terminal[endpoint] = best + information * 0.18;
        model.bestGoal[endpoint] = goal;
    }
}

struct Plan {
    GameOutput output;
    int predictedEnd[2];
    double baseScore;
    double scenarioMean;
    double scenarioP20;
    double scenarioDeviation;
    double robustScore;
};

struct BaseEvaluation {
    int end[2];
    double score;
};

double takeExpectedCoin(int cell, const Model& model, int touched[S],
                        double remaining[S], int& touchedCount) noexcept {
    int slot = -1;
    for (int index = 0; index < touchedCount; ++index) {
        if (touched[index] == cell) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        if (touchedCount >= S) return 0.0;
        slot = touchedCount++;
        touched[slot] = cell;
        remaining[slot] = model.gold[cell] * model.survival[cell];
    }
    if (remaining[slot] <= 0.0) return 0.0;
    // For a probability-weighted amount, +0.35 approximates the integer
    // ceiling without pretending that the uncertain amount is deterministic.
    const double pickup = std::min(
        remaining[slot], std::floor(remaining[slot] * 0.65 + 0.999999));
    remaining[slot] -= pickup;
    return pickup;
}

BaseEvaluation evaluateBase(const GameInput& input, const Model& model,
                            const GameOutput& output) noexcept {
    BaseEvaluation evaluation{{cellOf(input.my_units[0]),
                               cellOf(input.my_units[1])},
                              0.0};
    double held[2] = {static_cast<double>(clampGold(input.my_units_gold[0])),
                      static_cast<double>(clampGold(input.my_units_gold[1]))};
    int touched[S] = {-1, -1, -1, -1, -1, -1};
    double remaining[S]{};
    int touchedCount = 0;
    double pickup = 0.0;
    double loss = 0.0;
    double fragility = 0.0;
    double movement = 0.0;
    double exploration = 0.0;
    int blocked = 0;
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) {
                fragility += 0.015;
                continue;
            }
            const Position current = positionOf(evaluation.end[unit]);
            const int row = current.row + kDr[action];
            const int col = current.col + kDc[action];
            if (!inside(row, col)) {
                ++blocked;
                fragility += 2.2;
                continue;
            }
            const int target = cellOf(row, col);
            if (model.wall[target] || target == evaluation.end[unit ^ 1]) {
                ++blocked;
                fragility += 2.0;
                continue;
            }
            if (model.uncertain[target]) {
                fragility += 0.9 * model.observedWallRate;
            }
            fragility += model.enemyBlock[target] *
                         (1.1 + gMemory.cells[target].blockEvidence * 0.2);
            evaluation.end[unit] = target;
            movement += 1.0;
            exploration += model.information[target] * 0.055;
            const double gained = takeExpectedCoin(
                target, model, touched, remaining, touchedCount);
            pickup += gained;
            held[unit] += gained;
            if (model.bomb[target] > 0.0) {
                const double expectedLoss =
                    std::ceil(held[unit] * 0.10 - 1e-12) * model.bomb[target];
                held[unit] -= expectedLoss;
                loss += expectedLoss;
                fragility += model.bomb[target] * 1.8;
            }
            if (model.npcTrample[target] > 0.0) {
                const double expectedLoss =
                    std::ceil(held[unit] * 0.05 - 1e-12) *
                    model.npcTrample[target];
                held[unit] -= expectedLoss;
                loss += expectedLoss;
                fragility += model.npcTrample[target] * 0.8;
            }
        }
    }
    double terminal = model.terminal[evaluation.end[0]] +
                      model.terminal[evaluation.end[1]];
    if (model.bestGoal[evaluation.end[0]] ==
        model.bestGoal[evaluation.end[1]]) {
        terminal *= 0.82;
        fragility += 0.6;
    }
    const int separation = manhattan(evaluation.end[0], evaluation.end[1]);
    const bool differentRegions =
        regionOf(evaluation.end[0]) != regionOf(evaluation.end[1]);
    const double coverage = differentRegions ? 0.45 : 0.0;
    const double separationBonus = std::min(0.45, separation * 0.035);
    evaluation.score = pickup - loss * 1.17 + terminal * 0.58 +
                       exploration + movement * 0.035 + coverage +
                       separationBonus - fragility - blocked * 0.35;
    return evaluation;
}

bool planWorse(const Plan& first, const Plan& second) noexcept {
    if (first.baseScore != second.baseScore) {
        return first.baseScore < second.baseScore;
    }
    if (first.output.k != second.output.k) return first.output.k < second.output.k;
    if (first.output.order != second.output.order) {
        return first.output.order < second.output.order;
    }
    for (int index = 0; index < S; ++index) {
        if (first.output.actions[index] != second.output.actions[index]) {
            return first.output.actions[index] > second.output.actions[index];
        }
    }
    return false;
}

void siftUp(Plan heap[kTopPlans], int index) noexcept {
    while (index > 0) {
        const int parent = (index - 1) / 2;
        if (!planWorse(heap[index], heap[parent])) break;
        std::swap(heap[index], heap[parent]);
        index = parent;
    }
}

void siftDown(Plan heap[kTopPlans], int count, int index) noexcept {
    for (;;) {
        int worst = index;
        const int left = index * 2 + 1;
        const int right = left + 1;
        if (left < count && planWorse(heap[left], heap[worst])) worst = left;
        if (right < count && planWorse(heap[right], heap[worst])) worst = right;
        if (worst == index) return;
        std::swap(heap[index], heap[worst]);
        index = worst;
    }
}

void retainPlan(Plan heap[kTopPlans], int& count, const Plan& plan) noexcept {
    if (count < kTopPlans) {
        heap[count] = plan;
        siftUp(heap, count);
        ++count;
        return;
    }
    if (!planWorse(heap[0], plan)) return;
    heap[0] = plan;
    siftDown(heap, count, 0);
}

Plan makePlan(const GameInput& input, const Model& model,
              const GameOutput& output) noexcept {
    const BaseEvaluation evaluation = evaluateBase(input, model, output);
    Plan plan{};
    plan.output = output;
    plan.predictedEnd[0] = evaluation.end[0];
    plan.predictedEnd[1] = evaluation.end[1];
    plan.baseScore = evaluation.score;
    plan.scenarioMean = evaluation.score;
    plan.scenarioP20 = evaluation.score;
    plan.scenarioDeviation = 0.0;
    plan.robustScore = evaluation.score;
    return plan;
}

int generatePlans(const GameInput& input, const Model& model,
                  Plan plans[kTopPlans]) noexcept {
    int count = 0;
    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay}, 3, 0, 0};
    retainPlan(plans, count, makePlan(input, model, output));

    if constexpr (SLOW_PLAYER_EXHAUSTIVE != 0) {
        for (int code = 0; code < 15625; ++code) {
            int encoded = code;
            for (int index = 0; index < S; ++index) {
                output.actions[index] = encoded % 5;
                encoded /= 5;
            }
            for (int split = 0; split <= S; ++split) {
                output.k = split;
                for (int order = 0; order < 2; ++order) {
                    output.order = order;
                    retainPlan(plans, count, makePlan(input, model, output));
                }
            }
        }
    } else {
        // Test/sanitizer builds sample the whole action-code space with a
        // coprime stride.  They still exercise every split and both orders.
        constexpr int kSamples = 72;
        int code = (input.round * 3571 + 19) % 15625;
        for (int sample = 0; sample < kSamples; ++sample) {
            int encoded = code;
            for (int index = 0; index < S; ++index) {
                output.actions[index] = encoded % 5;
                encoded /= 5;
            }
            for (int split = 0; split <= S; ++split) {
                output.k = split;
                for (int order = 0; order < 2; ++order) {
                    output.order = order;
                    retainPlan(plans, count, makePlan(input, model, output));
                }
            }
            code = (code + 7919) % 15625;
        }
    }
    std::sort(plans, plans + count,
              [](const Plan& first, const Plan& second) noexcept {
                  return planWorse(second, first);
              });
    return count;
}

struct Scenario {
    int coins[kCells];
    std::uint8_t walls[kCells];
    std::uint8_t bombs[kCells];
    std::uint8_t npcCount[kCells];
    int enemies[2];
    int npcs[MAX_NPCS];
};

int sampleActorCell(const ActorBelief& actor, const Model& model,
                    Rng& rng) noexcept {
    if (actor.assigned) {
        double total = 0.0;
        for (int cell = 0; cell < kCells; ++cell) {
            if (!model.wall[cell]) total += actor.probability[cell];
        }
        if (total > 1e-12) {
            double choice = rng.unit() * total;
            for (int cell = 0; cell < kCells; ++cell) {
                if (model.wall[cell]) continue;
                choice -= actor.probability[cell];
                if (choice <= 0.0) return cell;
            }
        }
    }
    for (int attempt = 0; attempt < kCells * 2; ++attempt) {
        const int cell = rng.range(kCells);
        if (!model.wall[cell]) return cell;
    }
    return cellOf(8, 8);
}

int chooseScenarioMap(Rng& rng) noexcept {
    const int count = bitCount(gMemory.templateMask);
    if (count <= 0) return -1;
    int choice = rng.range(count);
    for (int map = 0; map < 4; ++map) {
        if ((gMemory.templateMask & (1U << static_cast<unsigned>(map))) == 0U) {
            continue;
        }
        if (choice-- == 0) return map;
    }
    return firstTemplate(gMemory.templateMask);
}

int stochasticCoin(double mean, Rng& rng) noexcept {
    mean = clampDouble(mean, 0.0, 100000000.0);
    if (mean <= 0.0) return 0;
    if (mean < 1.0) return rng.unit() < mean ? 1 : 0;
    const double multiplier = 0.45 + rng.unit() * 1.10;
    const double sampled = mean * multiplier;
    return sampled >= 100000000.0
               ? 100000000
               : static_cast<int>(sampled + 0.5);
}

bool occupiedByPlayer(int cell, const int positions[2], int ignore) noexcept {
    for (int index = 0; index < 2; ++index) {
        if (index != ignore && positions[index] == cell) return true;
    }
    return false;
}

int bestCoinTarget(const Scenario& scenario, int from, const Model& model,
                   Rng& rng, bool npc) noexcept {
    int best = from;
    double bestScore = npc ? 0.15 : 0.05;
    for (int cell = 0; cell < kCells; ++cell) {
        if (scenario.walls[cell] || scenario.coins[cell] <= 0) continue;
        const std::uint16_t distance = model.distance[from][cell];
        if (distance == kDistInf) continue;
        const double amount = scenario.coins[cell];
        double score = amount / (1.0 + distance * (npc ? 0.30 : 0.22));
        score *= 0.92 + rng.unit() * 0.16;
        if (score > bestScore) {
            bestScore = score;
            best = cell;
        }
    }
    return best;
}

int nextToward(const Scenario& scenario, int from, int goal,
               const Model& model, Rng& rng, const int blockers[2],
               int ignoreBlocker) noexcept {
    const Position p = positionOf(from);
    int choices[5];
    double scores[5];
    int count = 0;
    choices[count] = from;
    scores[count++] = goal == from ? 0.5 : -0.3;
    for (int action = 0; action < 4; ++action) {
        const int row = p.row + kDr[action];
        const int col = p.col + kDc[action];
        if (!inside(row, col)) continue;
        const int target = cellOf(row, col);
        if (scenario.walls[target] ||
            occupiedByPlayer(target, blockers, ignoreBlocker)) {
            continue;
        }
        const std::uint16_t distance = model.distance[target][goal];
        double score = distance == kDistInf ? -1000.0 : -distance;
        score += scenario.coins[target] * 0.34;
        score += rng.unit() * 0.12;
        choices[count] = target;
        scores[count++] = score;
    }
    int best = 0;
    for (int index = 1; index < count; ++index) {
        if (scores[index] > scores[best]) best = index;
    }
    if (count > 1 && rng.unit() < 0.10) best = 1 + rng.range(count - 1);
    return choices[best];
}

int consumeScenarioCoin(Scenario& scenario, int cell) noexcept {
    const int pickup = pickup65(scenario.coins[cell]);
    scenario.coins[cell] -= pickup;
    return pickup;
}

int moveScenarioPlayer(Scenario& scenario, int positions[2], int unit,
                       int steps, const int fixedBlockers[2],
                       const Model& model, Rng& rng) noexcept {
    int pickup = 0;
    for (int step = 0; step < steps; ++step) {
        const int goal = bestCoinTarget(scenario, positions[unit], model, rng,
                                        false);
        int blockers[2] = {positions[0], positions[1]};
        int target = nextToward(scenario, positions[unit], goal, model, rng,
                                blockers, unit);
        if (target == fixedBlockers[0] || target == fixedBlockers[1]) {
            target = positions[unit];
        }
        if (target != positions[unit]) {
            positions[unit] = target;
            pickup += consumeScenarioCoin(scenario, target);
            if (scenario.bombs[target] != 0U) scenario.bombs[target] = 0U;
        }
    }
    return pickup;
}

void moveScenarioNpc(Scenario& scenario, int& position, const Model& model,
                     Rng& rng) noexcept {
    int noBlockers[2] = {-1, -1};
    for (int step = 0; step < 3; ++step) {
        if (rng.unit() < 0.18) continue;
        const int goal = bestCoinTarget(scenario, position, model, rng, true);
        const int target = nextToward(scenario, position, goal, model, rng,
                                      noBlockers, -1);
        if (target != position) {
            position = target;
            (void)consumeScenarioCoin(scenario, target);
        }
    }
}

void makeScenario(const GameInput& input, const Model& model, Rng& rng,
                  Scenario& scenario) noexcept {
    const int sampledMap = chooseScenarioMap(rng);
    for (int cell = 0; cell < kCells; ++cell) {
        const Terrain observed = gMemory.cells[cell].terrain;
        bool wall = observed == kTerrainWall;
        if (observed == kTerrainUnknown) {
            if (sampledMap >= 0) {
                wall = templateWall(sampledMap, cell);
            } else {
                wall = rng.unit() < model.observedWallRate;
            }
        }
        scenario.walls[cell] = wall ? 1U : 0U;
        scenario.coins[cell] = wall ? 0 : stochasticCoin(model.gold[cell], rng);
        const Position p = positionOf(cell);
        const int visible = input.grid[p.row][p.col];
        if (!wall && visible > 0) scenario.coins[cell] = clampGold(visible);
        scenario.bombs[cell] =
            !wall && rng.unit() < model.bomb[cell] ? 1U : 0U;
        scenario.npcCount[cell] = 0U;
    }
    const int own[2] = {cellOf(input.my_units[0]), cellOf(input.my_units[1])};
    scenario.walls[own[0]] = scenario.walls[own[1]] = 0U;
    scenario.enemies[0] = sampleActorCell(gMemory.enemies[0], model, rng);
    scenario.enemies[1] = sampleActorCell(gMemory.enemies[1], model, rng);
    if (scenario.enemies[0] == scenario.enemies[1] ||
        scenario.enemies[0] == own[0] || scenario.enemies[0] == own[1]) {
        scenario.enemies[0] = cellOf(0, 16);
    }
    if (scenario.enemies[1] == scenario.enemies[0] ||
        scenario.enemies[1] == own[0] || scenario.enemies[1] == own[1]) {
        scenario.enemies[1] = cellOf(16, 0);
    }
    scenario.walls[scenario.enemies[0]] = 0U;
    scenario.walls[scenario.enemies[1]] = 0U;
    const int split = rng.range(S + 1);
    const int order = rng.range(2);
    const int fixedOwn[2] = {own[0], own[1]};
    (void)moveScenarioPlayer(scenario, scenario.enemies, order,
                             order == 0 ? split : S - split, fixedOwn, model,
                             rng);
    (void)moveScenarioPlayer(scenario, scenario.enemies, order ^ 1,
                             order == 0 ? S - split : split, fixedOwn, model,
                             rng);

    for (int index = 0; index < MAX_NPCS; ++index) {
        scenario.npcs[index] = sampleActorCell(gMemory.npcs[index], model, rng);
        scenario.walls[scenario.npcs[index]] = 0U;
        moveScenarioNpc(scenario, scenario.npcs[index], model, rng);
        std::uint8_t& count = scenario.npcCount[scenario.npcs[index]];
        if (count < 255U) ++count;
    }
}

struct ScenarioEvaluation {
    int end[2];
    int pickup;
    int loss;
    int blocked;
    double score;
};

int scenarioCoinAt(const Scenario& scenario, int cell, int touched[S],
                   int remaining[S], int& touchedCount) noexcept {
    int slot = -1;
    for (int index = 0; index < touchedCount; ++index) {
        if (touched[index] == cell) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        if (touchedCount >= S) return 0;
        slot = touchedCount++;
        touched[slot] = cell;
        remaining[slot] = scenario.coins[cell];
    }
    const int pickup = pickup65(remaining[slot]);
    remaining[slot] -= pickup;
    return pickup;
}

ScenarioEvaluation evaluateScenario(const GameInput& input,
                                    const Model& model,
                                    const Scenario& scenario,
                                    const GameOutput& output) noexcept {
    ScenarioEvaluation evaluation{{cellOf(input.my_units[0]),
                                   cellOf(input.my_units[1])},
                                  0, 0, 0, 0.0};
    int held[2] = {clampGold(input.my_units_gold[0]),
                   clampGold(input.my_units_gold[1])};
    int touched[S] = {-1, -1, -1, -1, -1, -1};
    int remaining[S]{};
    int touchedCount = 0;
    int consumedBombs[S] = {-1, -1, -1, -1, -1, -1};
    int bombCount = 0;
    int moves = 0;
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position current = positionOf(evaluation.end[unit]);
            const int row = current.row + kDr[action];
            const int col = current.col + kDc[action];
            if (!inside(row, col)) {
                ++evaluation.blocked;
                continue;
            }
            const int target = cellOf(row, col);
            if (scenario.walls[target] != 0U ||
                target == evaluation.end[unit ^ 1] ||
                target == scenario.enemies[0] ||
                target == scenario.enemies[1]) {
                ++evaluation.blocked;
                continue;
            }
            evaluation.end[unit] = target;
            ++moves;
            const int gained = scenarioCoinAt(
                scenario, target, touched, remaining, touchedCount);
            evaluation.pickup += gained;
            held[unit] = std::min(100000000, held[unit] + gained);
            bool bombActive = scenario.bombs[target] != 0U;
            for (int old = 0; old < bombCount; ++old) {
                if (consumedBombs[old] == target) bombActive = false;
            }
            if (bombActive) {
                const int lost = loss10(held[unit]);
                held[unit] -= lost;
                evaluation.loss += lost;
                if (bombCount < S) consumedBombs[bombCount++] = target;
            }
            if (scenario.npcCount[target] >= 3U) {
                const int lost = loss5(held[unit]);
                held[unit] -= lost;
                evaluation.loss += lost;
            }
        }
    }
    double terminal = model.terminal[evaluation.end[0]] +
                      model.terminal[evaluation.end[1]];
    if (model.bestGoal[evaluation.end[0]] ==
        model.bestGoal[evaluation.end[1]]) {
        terminal *= 0.83;
    }
    evaluation.score = evaluation.pickup - 1.17 * evaluation.loss +
                       terminal * 0.54 + moves * 0.035 -
                       evaluation.blocked * 1.35 +
                       (regionOf(evaluation.end[0]) !=
                                regionOf(evaluation.end[1])
                            ? 0.4
                            : 0.0);
    return evaluation;
}

void evaluatePlansInScenarios(const GameInput& input, const Model& model,
                              const Scenario scenarios[kScenarioCount],
                              Plan plans[kTopPlans], int count) noexcept {
    const int ownGold = clampGold(input.my_units_gold[0]) +
                        clampGold(input.my_units_gold[1]);
    const int lead = ownGold - gMemory.visionSpent - clampGold(input.gold_opp);
    const double progress = clampDouble(input.round / 499.0, 0.0, 1.0);
    const double downsideWeight =
        lead > 0 ? 0.30 + 0.18 * progress : 0.14 + 0.05 * progress;
    for (int index = 0; index < count; ++index) {
        double values[kScenarioCount];
        double sum = 0.0;
        double sumSquares = 0.0;
        for (int scenario = 0; scenario < kScenarioCount; ++scenario) {
            const ScenarioEvaluation evaluation = evaluateScenario(
                input, model, scenarios[scenario], plans[index].output);
            values[scenario] = evaluation.score;
            sum += evaluation.score;
            sumSquares += evaluation.score * evaluation.score;
        }
        std::sort(values, values + kScenarioCount);
        const double mean = sum / kScenarioCount;
        const double variance =
            std::max(0.0, sumSquares / kScenarioCount - mean * mean);
        const double deviation = std::sqrt(variance);
        const int quantileIndex = (kScenarioCount - 1) / 5;
        double tail = 0.0;
        for (int sample = 0; sample <= quantileIndex; ++sample) {
            tail += values[sample];
        }
        tail /= quantileIndex + 1;
        plans[index].scenarioMean = mean;
        plans[index].scenarioP20 = tail;
        plans[index].scenarioDeviation = deviation;
        plans[index].robustScore =
            0.18 * plans[index].baseScore +
            (1.0 - downsideWeight) * mean + downsideWeight * tail -
            (lead > 0 ? 0.055 : 0.025) * deviation;
    }
    std::sort(plans, plans + count,
              [](const Plan& first, const Plan& second) noexcept {
                  if (first.robustScore != second.robustScore) {
                      return first.robustScore > second.robustScore;
                  }
                  return planWorse(second, first);
              });
}

struct RolloutWorld {
    Scenario board;
    int own[2];
    int held[2];
};

struct TurnGain {
    int pickup;
    int loss;
};

TurnGain applyCurrentOutput(RolloutWorld& world, const GameOutput& output,
                            Rng& rng, const Model& model) noexcept {
    TurnGain gain{0, 0};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position current = positionOf(world.own[unit]);
            const int row = current.row + kDr[action];
            const int col = current.col + kDc[action];
            if (!inside(row, col)) continue;
            const int target = cellOf(row, col);
            if (world.board.walls[target] != 0U ||
                target == world.own[unit ^ 1] ||
                target == world.board.enemies[0] ||
                target == world.board.enemies[1]) {
                continue;
            }
            world.own[unit] = target;
            const int pickup = consumeScenarioCoin(world.board, target);
            gain.pickup += pickup;
            world.held[unit] =
                std::min(100000000, world.held[unit] + pickup);
            if (world.board.bombs[target] != 0U) {
                const int loss = loss10(world.held[unit]);
                world.held[unit] -= loss;
                gain.loss += loss;
                world.board.bombs[target] = 0U;
            }
            if (world.board.npcCount[target] >= 3U) {
                const int loss = loss5(world.held[unit]);
                world.held[unit] -= loss;
                gain.loss += loss;
            }
            // Rarely model an unseen opponent holding a narrow target for a
            // second action.  It is deliberately a small perturbation because
            // the exact endpoint blockers were already sampled.
            if (rng.unit() < model.enemyBlock[target] * 0.03) break;
        }
    }
    return gain;
}

bool occupiedInWorld(const RolloutWorld& world, int cell) noexcept {
    if (world.own[0] == cell || world.own[1] == cell ||
        world.board.enemies[0] == cell || world.board.enemies[1] == cell) {
        return true;
    }
    for (int npc : world.board.npcs) {
        if (npc == cell) return true;
    }
    return false;
}

void addGold(RolloutWorld& world, int cell, int amount) noexcept {
    if (cell < 0 || cell >= kCells || amount <= 0 ||
        world.board.walls[cell] != 0U || world.board.bombs[cell] != 0U) {
        return;
    }
    world.board.coins[cell] =
        std::min(100000000, world.board.coins[cell] + amount);
}

int randomOpenCell(const RolloutWorld& world, Rng& rng, int region,
                   bool requireEmpty) noexcept {
    for (int attempt = 0; attempt < kCells * 3; ++attempt) {
        const int cell = rng.range(kCells);
        if (world.board.walls[cell] != 0U ||
            (region > 0 && regionOf(cell) != region) ||
            (requireEmpty && occupiedInWorld(world, cell))) {
            continue;
        }
        return cell;
    }
    return -1;
}

void spawnFutureGold(RolloutWorld& world, int futureRound,
                     const Model& model, Rng& rng) noexcept {
    double centralMean = 8.0;
    if (gMemory.regions[0].valid) {
        centralMean = clampDouble(
            gMemory.regions[0].generationEwma / 5.0, 1.0, 30.0);
    }
    int central = std::max(
        1, static_cast<int>(centralMean * (0.55 + rng.unit() * 0.90)));
    while (central > 0) {
        const int chunk = std::min(central, 1 + rng.range(4));
        addGold(world, randomOpenCell(world, rng, 1, false), chunk);
        central -= chunk;
    }

    double eventProbability = model.outerHazard;
    if (futureRound - gMemory.lastOuterEvent > 20) eventProbability = 0.18;
    if (rng.unit() < eventProbability) {
        const int total = std::max(
            80, static_cast<int>(gMemory.outerEventAmount *
                                 (0.84 + rng.unit() * 0.34)));
        int candidates[kCells];
        int count = 0;
        for (int cell = 0; cell < kCells; ++cell) {
            if (world.board.walls[cell] == 0U && regionOf(cell) != 1 &&
                model.hotspotProbability[cell] > 0.35) {
                candidates[count++] = cell;
            }
        }
        const int major = count > 0 ? candidates[rng.range(count)]
                                    : randomOpenCell(world, rng,
                                                     2 + rng.range(4), false);
        const int majorAmount = total * (72 + rng.range(17)) / 100;
        addGold(world, major, majorAmount);
        int remainder = total - majorAmount;
        while (remainder > 0) {
            const int chunk = std::min(remainder, 1 + rng.range(8));
            addGold(world,
                    randomOpenCell(world, rng, 2 + rng.range(4), false),
                    chunk);
            remainder -= chunk;
        }
    }
}

void refreshFutureBombs(RolloutWorld& world, int futureRound,
                        Rng& rng) noexcept {
    if (futureRound % 20 != 0) return;
    for (std::uint8_t& bomb : world.board.bombs) bomb = 0U;
    const int desired = 4 + rng.range(5);
    int placed = 0;
    for (int attempt = 0; attempt < kCells * 3 && placed < desired; ++attempt) {
        const int cell = randomOpenCell(world, rng, 0, true);
        if (cell < 0 || world.board.coins[cell] > 0 ||
            world.board.bombs[cell] != 0U) {
            continue;
        }
        world.board.bombs[cell] = 1U;
        ++placed;
    }
}

int moveFutureEnemies(RolloutWorld& world, const Model& model,
                      Rng& rng) noexcept {
    const int split = rng.range(S + 1);
    const int order = rng.range(2);
    const int fixedOwn[2] = {world.own[0], world.own[1]};
    int pickup = moveScenarioPlayer(
        world.board, world.board.enemies, order,
        order == 0 ? split : S - split, fixedOwn, model, rng);
    pickup += moveScenarioPlayer(
        world.board, world.board.enemies, order ^ 1,
        order == 0 ? S - split : split, fixedOwn, model, rng);
    return pickup;
}

void moveFutureNpcs(RolloutWorld& world, const Model& model,
                    Rng& rng) noexcept {
    for (std::uint8_t& count : world.board.npcCount) count = 0U;
    int order[MAX_NPCS] = {0, 1, 2, 3, 4, 5, 6};
    for (int index = MAX_NPCS - 1; index > 0; --index) {
        std::swap(order[index], order[rng.range(index + 1)]);
    }
    for (int turn = 0; turn < MAX_NPCS; ++turn) {
        const int index = order[turn];
        moveScenarioNpc(world.board, world.board.npcs[index], model, rng);
        std::uint8_t& count =
            world.board.npcCount[world.board.npcs[index]];
        if (count < 255U) ++count;
    }
}

double urgency(const RolloutWorld& world, int unit,
               const Model& model) noexcept {
    const int from = world.own[unit];
    double best = 0.1;
    for (int cell = 0; cell < kCells; ++cell) {
        if (world.board.walls[cell] != 0U || world.board.coins[cell] <= 0) {
            continue;
        }
        const std::uint16_t distance = model.distance[from][cell];
        if (distance == kDistInf) continue;
        const double value = world.board.coins[cell] /
                             (1.0 + static_cast<double>(distance) * 0.22);
        if (value > best) best = value;
    }
    return best;
}

TurnGain moveFutureOwnUnit(RolloutWorld& world, int unit, int steps,
                           const Model& model, Rng& rng) noexcept {
    TurnGain gain{0, 0};
    for (int step = 0; step < steps; ++step) {
        const int goal = bestCoinTarget(world.board, world.own[unit], model,
                                        rng, false);
        int ownBlockers[2] = {world.own[0], world.own[1]};
        int target = nextToward(world.board, world.own[unit], goal, model, rng,
                                ownBlockers, unit);
        if (target == world.board.enemies[0] ||
            target == world.board.enemies[1]) {
            target = world.own[unit];
        }
        if (target == world.own[unit]) continue;
        world.own[unit] = target;
        const int pickup = consumeScenarioCoin(world.board, target);
        gain.pickup += pickup;
        world.held[unit] = std::min(100000000, world.held[unit] + pickup);
        if (world.board.bombs[target] != 0U) {
            const int loss = loss10(world.held[unit]);
            world.held[unit] -= loss;
            gain.loss += loss;
            world.board.bombs[target] = 0U;
        }
        if (world.board.npcCount[target] >= 3U) {
            const int loss = loss5(world.held[unit]);
            world.held[unit] -= loss;
            gain.loss += loss;
        }
    }
    return gain;
}

TurnGain moveFutureOwn(RolloutWorld& world, const Model& model,
                       Rng& rng) noexcept {
    const double firstUrgency = urgency(world, 0, model);
    const double secondUrgency = urgency(world, 1, model);
    int split = static_cast<int>(
        std::lround(S * firstUrgency / (firstUrgency + secondUrgency)));
    split = std::max(1, std::min(S - 1, split));
    const int order = firstUrgency >= secondUrgency ? 0 : 1;
    TurnGain total{0, 0};
    TurnGain gain = moveFutureOwnUnit(
        world, order, order == 0 ? split : S - split, model, rng);
    total.pickup += gain.pickup;
    total.loss += gain.loss;
    gain = moveFutureOwnUnit(world, order ^ 1,
                             order == 0 ? S - split : split, model, rng);
    total.pickup += gain.pickup;
    total.loss += gain.loss;
    return total;
}

double rolloutFuture(const GameInput& input, const Model& model,
                     const Scenario& scenario, const Plan& plan,
                     std::uint64_t seed) noexcept {
    Rng rng(seed);
    RolloutWorld world{};
    world.board = scenario;
    world.own[0] = cellOf(input.my_units[0]);
    world.own[1] = cellOf(input.my_units[1]);
    world.held[0] = clampGold(input.my_units_gold[0]);
    world.held[1] = clampGold(input.my_units_gold[1]);
    (void)applyCurrentOutput(world, plan.output, rng, model);

    const int remainingRounds = std::max(0, 499 - input.round);
    const int horizon = std::min(remainingRounds, 7 + rng.range(6));
    double value = 0.0;
    double discount = 0.93;
    for (int turn = 1; turn <= horizon; ++turn) {
        const int futureRound = input.round + turn;
        spawnFutureGold(world, futureRound, model, rng);
        refreshFutureBombs(world, futureRound, rng);
        const int enemyPickup = moveFutureEnemies(world, model, rng);
        moveFutureNpcs(world, model, rng);
        const TurnGain own = moveFutureOwn(world, model, rng);
        value += discount *
                 (own.pickup - own.loss * 1.16 - enemyPickup * 0.32);
        discount *= 0.92;
    }
    value += discount * 0.18 *
             (model.terminal[world.own[0]] + model.terminal[world.own[1]]);
    return value;
}

int selectWithRollouts(const GameInput& input, const Model& model,
                       const Scenario scenarios[kScenarioCount],
                       Plan plans[kTopPlans], int count,
                       Clock::time_point deadline,
                       std::uint64_t baseSeed) noexcept {
    const int finalists = std::min(count, kFinalists);
    if (finalists <= 1 || kThinkUs <= 0) return 0;
    int visits[kFinalists]{};
    double means[kFinalists]{};
    double m2[kFinalists]{};
    int batch = 0;
    bool timeExpired = false;
    while (!timeExpired) {
        const int scenarioIndex = batch % kScenarioCount;
        const std::uint64_t commonSeed =
            baseSeed ^ (0xd1b54a32d192ed03ULL *
                        static_cast<std::uint64_t>(batch + 1));
        for (int index = 0; index < finalists; ++index) {
            if (Clock::now() >= deadline) {
                timeExpired = true;
                break;
            }
            const double sample = rolloutFuture(
                input, model, scenarios[scenarioIndex], plans[index],
                commonSeed);
            ++visits[index];
            const double delta = sample - means[index];
            means[index] += delta / visits[index];
            m2[index] += delta * (sample - means[index]);
        }
        ++batch;
    }
    int best = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (int index = 0; index < finalists; ++index) {
        const double variance = visits[index] > 1
                                    ? m2[index] / (visits[index] - 1)
                                    : 0.0;
        const double error = visits[index] > 0
                                 ? std::sqrt(std::max(0.0, variance) /
                                             visits[index])
                                 : 1000.0;
        const double score = plans[index].robustScore +
                             (visits[index] > 0 ? means[index] * 0.31 : 0.0) -
                             error * 0.08;
        if (score > bestScore) {
            bestScore = score;
            best = index;
        }
    }
    return best;
}

int chooseVision(const GameInput& input, const Model& model,
                 const Plan& plan) noexcept {
    if (input.round >= 499) return 0;
    const int gross = clampGold(input.my_units_gold[0]) +
                      clampGold(input.my_units_gold[1]);
    const int maximumSpend = std::max(9, gross / 12);
    if (gMemory.visionSpent + 2 > maximumSpend) return 0;
    bool counted[kCells]{};
    double ring3 = 0.0;
    double ring4 = 0.0;
    for (int unit = 0; unit < 2; ++unit) {
        const Position center = positionOf(plan.predictedEnd[unit]);
        for (int dr = -4; dr <= 4; ++dr) {
            for (int dc = -4; dc <= 4; ++dc) {
                const int radius = std::max(absInt(dr), absInt(dc));
                if (radius <= 2 || radius > 4) continue;
                const int row = center.row + dr;
                const int col = center.col + dc;
                if (!inside(row, col)) continue;
                const int cell = cellOf(row, col);
                if (counted[cell]) continue;
                counted[cell] = true;
                const CellMemory& memory = gMemory.cells[cell];
                const int age = std::max(0, input.round - memory.lastSeen);
                double value = memory.terrain == kTerrainUnknown ? 0.10 : 0.02;
                value += std::min(0.12, age * 0.006);
                value += std::min(0.24, std::sqrt(model.gold[cell]) * 0.025);
                value += model.hotspotProbability[cell] *
                         model.outerHazard * 0.22;
                if (radius == 3) {
                    ring3 += value;
                } else {
                    ring4 += value;
                }
            }
        }
    }
    double context = 1.0;
    if (input.snapshot_valid == 1) context += 0.18;
    if (model.outerHazard > 0.10) context += 0.16;
    const double value7 = ring3 * context;
    const double value9 = (ring3 + ring4) * context;
    if (gMemory.visionSpent + 3 <= maximumSpend && value9 > 3.75 &&
        value9 - value7 > 0.75) {
        return 2;
    }
    return value7 > 2.65 ? 1 : 0;
}

void rememberPlan(const GameInput& input, const GameOutput& output) noexcept {
    gMemory.previous.valid = true;
    gMemory.previous.round = input.round;
    gMemory.previous.starts[0] = input.my_units[0];
    gMemory.previous.starts[1] = input.my_units[1];
    gMemory.previous.output = output;
    if (output.vp == 1) gMemory.visionSpent += 2;
    if (output.vp == 2) gMemory.visionSpent += 3;
}

}  // namespace

GameOutput decide(const GameInput* input) noexcept {
    if (!validInput(input)) return fallback();
    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline =
        start + std::chrono::microseconds(kThinkUs > 0 ? kThinkUs : 0);

    const bool discontinuity =
        !gMemory.initialized || input->round == 0 ||
        input->round != gMemory.lastRound + 1;
    if (discontinuity) resetMemory(*input);
    updateMemory(*input);

    Model model{};
    buildModel(*input, model);
    Plan plans[kTopPlans];
    const int planCount = generatePlans(*input, model, plans);
    if (planCount <= 0) return fallback();

    Scenario scenarios[kScenarioCount];
    const std::uint64_t seed =
        hashInput(*input) ^
        (gMemory.gameSerial * 0xa0761d6478bd642fULL);
    Rng scenarioRng(seed);
    for (int index = 0; index < kScenarioCount; ++index) {
        makeScenario(*input, model, scenarioRng, scenarios[index]);
    }
    evaluatePlansInScenarios(*input, model, scenarios, plans, planCount);
    const int chosen = selectWithRollouts(*input, model, scenarios, plans,
                                          planCount, deadline, seed);
    GameOutput output = plans[chosen].output;
    output.vp = chooseVision(*input, model, plans[chosen]);
    rememberPlan(*input, output);
    return output;
}

}  // namespace slow_player
