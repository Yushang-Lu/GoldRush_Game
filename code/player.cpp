#include "game_api.h"

#include <climits>
#include <cstdint>
#include <cstring>

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(Position) == 8, "Position ABI mismatch");
static_assert(sizeof(NpcInfo) == 12, "NpcInfo ABI mismatch");
static_assert(sizeof(RegionStat) == 28, "RegionStat ABI mismatch");
static_assert(sizeof(Snapshot) == 148, "Snapshot ABI mismatch");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

namespace {

constexpr int kSide = GRID_SIZE;
constexpr int kCells = GRID_SIZE * GRID_SIZE;
constexpr int kWords = (kCells + 31) / 32;
constexpr int kStay = 4;
constexpr int kDr[5] = {-1, 1, 0, 0, 0};
constexpr int kDc[5] = {0, 0, -1, 1, 0};

struct Offset {
    std::int8_t dr;
    std::int8_t dc;
    std::int8_t delta;
    std::uint8_t distance;
};

constexpr Offset kLocal25[25] = {
    {-2, -2, -36, 4}, {-2, -1, -35, 3}, {-2, 0, -34, 2},
    {-2, 1, -33, 3},  {-2, 2, -32, 4},  {-1, -2, -19, 3},
    {-1, -1, -18, 2}, {-1, 0, -17, 1},  {-1, 1, -16, 2},
    {-1, 2, -15, 3},  {0, -2, -2, 2},   {0, -1, -1, 1},
    {0, 0, 0, 0},     {0, 1, 1, 1},     {0, 2, 2, 2},
    {1, -2, 15, 3},   {1, -1, 16, 2},   {1, 0, 17, 1},
    {1, 1, 18, 2},    {1, 2, 19, 3},    {2, -2, 32, 4},
    {2, -1, 33, 3},   {2, 0, 34, 2},    {2, 1, 35, 3},
    {2, 2, 36, 4},
};

inline bool inside(int row, int col) noexcept {
    return static_cast<unsigned>(row) < static_cast<unsigned>(kSide) &&
           static_cast<unsigned>(col) < static_cast<unsigned>(kSide);
}

inline bool validPosition(const Position& position) noexcept {
    return inside(position.row, position.col);
}

constexpr int cellOf(int row, int col) noexcept {
    return row * kSide + col;
}

inline int absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

inline int clampGold(int value) noexcept {
    return value <= 0 ? 0 : (value < 1000000 ? value : 1000000);
}

inline int remainingGold(int gold) noexcept {
    return gold > 0
               ? static_cast<int>((static_cast<std::uint64_t>(
                                        static_cast<unsigned>(gold)) *
                                    7U) /
                                   20U)
               : 0;
}

inline int pickupGold(int gold) noexcept {
    return gold > 0 ? gold - remainingGold(gold) : 0;
}

struct Candidate {
    std::uint16_t cell;
    std::uint8_t distance;
    std::uint8_t padding;
};

struct LocalScan {
    Candidate candidates[25];
    int opportunity;
    int projected[S];
    int count;
};

struct Target {
    std::int16_t cell;
    int gold;
};

struct Route {
    std::uint8_t actions[S];
    std::int16_t end;
};

struct GoldState {
    std::int16_t cells[S];
    int values[S];
    int count;
    int expectedGain;
};

struct PersistentState {
    bool initialized;
    std::uint8_t slowEvidence;
    std::uint16_t padding;
    int lastRound;
    int bombCycle;
    std::int16_t regionTarget;
    std::int16_t statePadding;
    int regionUntil;
    int predictionRound;
    std::int64_t predictedHeld;
    int predictedGain;
    std::uint32_t bombs[kWords];
};

PersistentState gState{};

GameOutput fallbackOutput() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

void resetState() noexcept {
    gState.initialized = true;
    gState.slowEvidence = 0U;
    gState.padding = 0U;
    gState.lastRound = -1;
    gState.bombCycle = -1;
    gState.regionTarget = -1;
    gState.statePadding = 0;
    gState.regionUntil = -1;
    gState.predictionRound = -1;
    gState.predictedHeld = 0;
    gState.predictedGain = 0;
    std::memset(gState.bombs, 0, sizeof(gState.bombs));
}

void beginTurn(const GameInput& input) noexcept {
    const bool discontinuity =
        gState.initialized && gState.lastRound >= 0 &&
        input.round != gState.lastRound + 1;
    if (!gState.initialized || input.round == 0 || discontinuity) {
        resetState();
    }

    const int cycle = input.round / 20;
    if (cycle != gState.bombCycle) {
        std::memset(gState.bombs, 0, sizeof(gState.bombs));
        gState.bombCycle = cycle;
    }
}

bool likelySlowHand(const GameInput& input) noexcept {
    const std::int64_t held =
        static_cast<std::int64_t>(input.my_units_gold[0]) +
        static_cast<std::int64_t>(input.my_units_gold[1]);
    if (gState.predictionRound == input.round - 1 &&
        gState.predictedGain >= 2) {
        const std::int64_t actualGain = held - gState.predictedHeld;
        if (actualGain >= 0 && actualGain < gState.predictedGain) {
            if (gState.slowEvidence < 8U) ++gState.slowEvidence;
        } else if (actualGain >= gState.predictedGain &&
                   gState.slowEvidence == 1U) {
            --gState.slowEvidence;
        }
    }
    return gState.slowEvidence >= 2U;
}

void rememberExpectedGain(const GameInput& input, int gain) noexcept {
    gState.predictionRound = input.round;
    gState.predictedHeld =
        static_cast<std::int64_t>(input.my_units_gold[0]) +
        static_cast<std::int64_t>(input.my_units_gold[1]);
    gState.predictedGain = gain;
}

void updateSnapshotTarget(const GameInput& input) noexcept {
    if (input.snapshot_valid != 1 || input.round >= 490) return;
    int centerScore = INT_MIN;
    int bestOuterScore = INT_MIN;
    int bestOuterId = -1;
    for (int index = 0; index < REGION_COUNT; ++index) {
        const RegionStat& stat = input.snapshot.regions[index];
        if (stat.id < 1 || stat.id > REGION_COUNT) continue;
        const int area = stat.id == 1 ? 81 : 52;
        const int remaining = stat.gold_remaining <= 0
                                  ? 0
                                  : (stat.gold_remaining < 1000000
                                         ? stat.gold_remaining
                                         : 1000000);
        const int generated = stat.gold_generated <= 0
                                  ? 0
                                  : (stat.gold_generated < 1000000
                                         ? stat.gold_generated
                                         : 1000000);
        const int collected = stat.gold_collected <= 0
                                  ? 0
                                  : (stat.gold_collected < 1000000
                                         ? stat.gold_collected
                                         : 1000000);
        const int occupants = stat.occupants <= 0
                                  ? 0
                                  : (stat.occupants < 100 ? stat.occupants
                                                          : 100);
        const int flow = generated > collected ? generated - collected : 0;
        const int score = (remaining * 8 + flow * 2) / area - occupants * 3;
        if (stat.id == 1) {
            centerScore = score;
        } else if (score > bestOuterScore) {
            bestOuterScore = score;
            bestOuterId = stat.id;
        }
    }
    if (bestOuterId < 0 || centerScore == INT_MIN ||
        bestOuterScore < centerScore + 30) {
        return;
    }
    static constexpr std::int16_t regionCells[REGION_COUNT + 1] = {
        -1, 144, 42, 136, 246, 152};
    gState.regionTarget = regionCells[bestOuterId];
    gState.regionUntil = input.round + 9;
}

inline void setBit(std::uint32_t bits[kWords], int cell) noexcept {
    bits[cell >> 5] |= std::uint32_t{1} << (cell & 31);
}

inline void clearBit(std::uint32_t bits[kWords], int cell) noexcept {
    bits[cell >> 5] &= ~(std::uint32_t{1} << (cell & 31));
}

inline bool testBit(const std::uint32_t bits[kWords], int cell) noexcept {
    return (bits[cell >> 5] &
            (std::uint32_t{1} << (cell & 31))) != 0U;
}

void markPreempted(std::uint32_t bits[kWords], int row, int col) noexcept {
    const int cell = cellOf(row, col);
    setBit(bits, cell);
    if (row > 0) setBit(bits, cell - kSide);
    if (row + 1 < kSide) setBit(bits, cell + kSide);
    if (col > 0) setBit(bits, cell - 1);
    if (col + 1 < kSide) setBit(bits, cell + 1);
}

void prepareBoard(const GameInput& input,
                  std::uint32_t forbidden[kWords], bool slowHand,
                  std::uint32_t preempted[kWords]) noexcept {
    if (slowHand) {
        std::memset(preempted, 0, sizeof(std::uint32_t) * kWords);
        for (int index = 0; index < 2; ++index) {
            const Position enemy = input.visible_enemies[index];
            if (validPosition(enemy)) {
                markPreempted(preempted, enemy.row, enemy.col);
            }
        }
        for (int unit = 0; unit < 2; ++unit) {
            clearBit(preempted,
                     cellOf(input.my_units[unit].row,
                            input.my_units[unit].col));
        }
    }
    std::memset(forbidden, 0, sizeof(std::uint32_t) * kWords);
    int npcCells[MAX_NPCS]{};
    std::uint8_t npcCounts[MAX_NPCS]{};
    int distinctNpcs = 0;
    int npcLimit = input.num_visible_npcs;
    if (npcLimit < 0) npcLimit = 0;
    if (npcLimit > MAX_NPCS) npcLimit = MAX_NPCS;
    for (int index = 0; index < npcLimit; ++index) {
        const NpcInfo& npc = input.visible_npcs[index];
        if (npc.id == 0 || !validPosition(npc.pos)) continue;
        const int cell = cellOf(npc.pos.row, npc.pos.col);
        if (slowHand) {
            markPreempted(preempted, npc.pos.row, npc.pos.col);
        }
        int slot = 0;
        while (slot < distinctNpcs && npcCells[slot] != cell) ++slot;
        if (slot == distinctNpcs) {
            npcCells[slot] = cell;
            npcCounts[slot] = 0U;
            ++distinctNpcs;
        }
        if (++npcCounts[slot] >= 3U) setBit(forbidden, cell);
    }
    for (int index = 0; index < 2; ++index) {
        const Position enemy = input.visible_enemies[index];
        if (validPosition(enemy)) {
            setBit(forbidden, cellOf(enemy.row, enemy.col));
        }
    }
    gState.lastRound = input.round;
}

inline bool traversable(int cell, const std::uint32_t forbidden[kWords],
                        bool allowFog, const int ground[kCells]) noexcept {
    if (testBit(forbidden, cell) || ground[cell] == -1 ||
        ground[cell] == -3) {
        return false;
    }
    return allowFog || ground[cell] != -5;
}

inline bool hasEntry(int row, int col,
                     const int ground[kCells]) noexcept {
    const int cell = cellOf(row, col);
    if (row > 0 && ground[cell - kSide] != -1 &&
        ground[cell - kSide] != -3) {
        return true;
    }
    if (row + 1 < kSide && ground[cell + kSide] != -1 &&
        ground[cell + kSide] != -3) {
        return true;
    }
    if (col > 0 && ground[cell - 1] != -1 && ground[cell - 1] != -3) {
        return true;
    }
    if (col + 1 < kSide && ground[cell + 1] != -1 &&
        ground[cell + 1] != -3) {
        return true;
    }
    return false;
}

void scanLocal(const GameInput& input, int unit,
               const std::uint32_t forbidden[kWords],
               int ground[kCells], bool slowHand,
               std::uint32_t preempted[kWords],
               LocalScan& scan) noexcept {
    scan.opportunity = 0;
    scan.count = 0;
    for (int index = 0; index < S; ++index) scan.projected[index] = 0;
    const Position start = input.my_units[unit];
    const int startCell = cellOf(start.row, start.col);
    for (const Offset& offset : kLocal25) {
        const int row = start.row + offset.dr;
        const int col = start.col + offset.dc;
        if (!inside(row, col)) continue;
        const int cell = startCell + offset.delta;
        const int observed = ground[cell];
        if (observed == -3) setBit(gState.bombs, cell);
        int value = clampGold(observed);
        if (value > 0 && slowHand && testBit(preempted, cell)) {
            value = remainingGold(value);
            ground[cell] = value;
            clearBit(preempted, cell);
        }
        if (value <= 0 || testBit(forbidden, cell)) {
            continue;
        }
        if (offset.distance != 0 &&
            !hasEntry(row, col, ground)) {
            continue;
        }

        const int remain1 = remainingGold(value);
        const int take1 = value - remain1;
        const int remain2 = remainingGold(remain1);
        const int prefix2 = take1 + remain1 - remain2;
        const int prefix3 = prefix2 + remain2 - remainingGold(remain2);
        scan.opportunity += take1;

        const int distance = offset.distance;
        switch (distance) {
        case 0:
        case 2:
            if (take1 > scan.projected[1]) scan.projected[1] = take1;
            if (take1 > scan.projected[2]) scan.projected[2] = take1;
            if (prefix2 > scan.projected[3]) scan.projected[3] = prefix2;
            if (prefix2 > scan.projected[4]) scan.projected[4] = prefix2;
            if (prefix3 > scan.projected[5]) scan.projected[5] = prefix3;
            break;
        case 1:
            if (take1 > scan.projected[0]) scan.projected[0] = take1;
            if (take1 > scan.projected[1]) scan.projected[1] = take1;
            if (prefix2 > scan.projected[2]) scan.projected[2] = prefix2;
            if (prefix2 > scan.projected[3]) scan.projected[3] = prefix2;
            if (prefix3 > scan.projected[4]) scan.projected[4] = prefix3;
            if (prefix3 > scan.projected[5]) scan.projected[5] = prefix3;
            break;
        case 3:
            if (take1 > scan.projected[2]) scan.projected[2] = take1;
            if (take1 > scan.projected[3]) scan.projected[3] = take1;
            if (prefix2 > scan.projected[4]) scan.projected[4] = prefix2;
            if (prefix2 > scan.projected[5]) scan.projected[5] = prefix2;
            break;
        default:
            if (take1 > scan.projected[3]) scan.projected[3] = take1;
            if (take1 > scan.projected[4]) scan.projected[4] = take1;
            if (prefix2 > scan.projected[5]) scan.projected[5] = prefix2;
            break;
        }
        scan.candidates[scan.count++] = Candidate{
            static_cast<std::uint16_t>(cell), offset.distance, 0U};
    }
}

Target selectTarget(const Position start, const LocalScan& scan, int budget,
                    const int ground[kCells], int forbidden,
                    const GoldState& goldState) noexcept {
    Target best{static_cast<std::int16_t>(cellOf(start.row, start.col)), 0};
    Target approach = best;
    int bestProjected = 0;
    int bestFirst = 0;
    int bestDistance = INT_MAX;
    int approachFirst = 0;
    for (int index = 0; index < scan.count; ++index) {
        const Candidate& candidate = scan.candidates[index];
        const int cell = candidate.cell;
        if (cell == forbidden) continue;
        int value = clampGold(ground[cell]);
        for (int changed = 0; changed < goldState.count; ++changed) {
            if (goldState.cells[changed] == cell) {
                value = goldState.values[changed];
                break;
            }
        }
        if (value <= 0) continue;
        const int distance = candidate.distance;
        const int first = pickupGold(value);
        if (distance == budget + 1) {
            if (first > approachFirst) {
                approach = Target{static_cast<std::int16_t>(cell), value};
                approachFirst = first;
            }
            continue;
        }
        if (distance > budget || (distance == 0 && budget < 2)) continue;
        const int entries = distance == 0 ? budget / 2
                                          : 1 + (budget - distance) / 2;
        int remaining = value;
        int projected = 0;
        for (int entry = 0; entry < entries && entry < 3; ++entry) {
            const int take = pickupGold(remaining);
            projected += take;
            remaining -= take;
        }
        if (projected > bestProjected ||
            (projected == bestProjected &&
             (first > bestFirst ||
              (first == bestFirst && distance < bestDistance)))) {
            best = Target{static_cast<std::int16_t>(cell), value};
            bestProjected = projected;
            bestFirst = first;
            bestDistance = distance;
        }
    }
    return bestProjected > 0 ? best : approach;
}

Target macroTarget(const GameInput& input, int unit) noexcept {
    static constexpr std::int16_t patrol[8][2] = {
        {109, 179}, {111, 177}, {129, 159}, {163, 125},
        {179, 109}, {177, 111}, {159, 129}, {125, 163},
    };
    const Position start = input.my_units[unit];
    if (unit == 1 && input.round <= gState.regionUntil &&
        gState.regionTarget >= 0) {
        return Target{gState.regionTarget, 0};
    }
    if (start.row < 4 || start.row > 12 || start.col < 4 || start.col > 12) {
        return Target{static_cast<std::int16_t>(
                          unit == 0 ? cellOf(7, 8) : cellOf(9, 8)),
                      0};
    }
    return Target{patrol[(input.round >> 2) & 7][unit], 0};
}

int chooseAllocation(const LocalScan& scan0, const LocalScan& scan1) noexcept {
    int best = 3;
    int bestValue = -1;
    int proportional = 3;
    if (scan0.opportunity > scan1.opportunity) {
        proportional = scan0.opportunity > 2 * scan1.opportunity ? 5 : 4;
    } else if (scan1.opportunity > scan0.opportunity) {
        proportional = scan1.opportunity > 2 * scan0.opportunity ? 1 : 2;
    }
    int bestTie = INT_MAX;
    for (int allocation = 1; allocation <= 5; ++allocation) {
        const int value = scan0.projected[allocation - 1] +
                          scan1.projected[5 - allocation];
        const int tie = absInt(allocation - proportional);
        if (value > bestValue || (value == bestValue && tie < bestTie)) {
            best = allocation;
            bestValue = value;
            bestTie = tie;
        }
    }
    if (scan1.opportunity == 0 &&
        scan0.projected[5] >= bestValue + 2) {
        best = 6;
        bestValue = scan0.projected[5];
    }
    if (scan0.opportunity == 0 &&
        scan1.projected[5] >= bestValue + 2) {
        best = 0;
    }
    return best;
}

inline int distanceTo(int cell, int target) noexcept {
    const int row = cell / kSide;
    const int col = cell - row * kSide;
    const int targetRow = target / kSide;
    const int targetCol = target - targetRow * kSide;
    return absInt(row - targetRow) + absInt(col - targetCol);
}

int chooseStep(int current, int target, int macro, int blocked,
               bool allowFog, int previous,
               const std::uint32_t forbidden[kWords],
               const int ground[kCells],
               const GoldState& goldState) noexcept {
    const int row = current / kSide;
    const int col = current - row * kSide;
    int bestAction = kStay;
    std::int64_t bestScore = LLONG_MIN;
    for (int action = 0; action < 4; ++action) {
        const int nextRow = row + kDr[action];
        const int nextCol = col + kDc[action];
        if (!inside(nextRow, nextCol)) continue;
        const int next = cellOf(nextRow, nextCol);
        if (next == blocked ||
            !traversable(next, forbidden, allowFog, ground)) {
            continue;
        }
        const int destination = target >= 0 ? target : macro;
        std::int64_t score =
            -static_cast<std::int64_t>(distanceTo(next, destination)) * 256;
        int value = clampGold(ground[next]);
        for (int changed = 0; changed < goldState.count; ++changed) {
            if (goldState.cells[changed] == next) {
                value = goldState.values[changed];
                break;
            }
        }
        if (value > 0) {
            score += static_cast<std::int64_t>(pickupGold(value)) * 4096;
        }
        if (ground[next] == -5) score -= 16;
        if (previous >= 0 && (previous ^ 1) == action) score -= 8;
        if (score > bestScore) {
            bestScore = score;
            bestAction = action;
        }
    }
    return bestAction;
}

Route makeRoute(const GameInput& input, int unit, Target target, Target macro,
                int blocked, int actionCount,
                const std::uint32_t forbidden[kWords],
                int ground[kCells],
                GoldState& goldState) noexcept {
    Route route{{kStay, kStay, kStay, kStay, kStay, kStay},
                static_cast<std::int16_t>(cellOf(input.my_units[unit].row,
                                                 input.my_units[unit].col))};
    int current = route.end;
    int previous = -1;
    const bool allowFog = input.my_units_gold[unit] < 200;
    for (int step = 0; step < actionCount; ++step) {
        int destination = target.gold > 0 ? target.cell : macro.cell;
        if (target.gold > 0 && current == target.cell) {
            destination = macro.cell;
        }
        const int action = chooseStep(current, destination, macro.cell,
                                      blocked, allowFog, previous, forbidden,
                                      ground, goldState);
        route.actions[step] = static_cast<std::uint8_t>(action);
        if (action == kStay) continue;
        const int row = current / kSide;
        const int col = current - row * kSide;
        const int next = cellOf(row + kDr[action], col + kDc[action]);
        current = next;
        int value = clampGold(ground[current]);
        int changedIndex = -1;
        for (int changed = 0; changed < goldState.count; ++changed) {
            if (goldState.cells[changed] == current) {
                value = goldState.values[changed];
                changedIndex = changed;
                break;
            }
        }
        if (value > 0) {
            const int take = pickupGold(value);
            goldState.expectedGain += take;
            if (changedIndex < 0) {
                changedIndex = goldState.count++;
                goldState.cells[changedIndex] =
                    static_cast<std::int16_t>(current);
            }
            goldState.values[changedIndex] = value - take;
        }
        previous = action;
    }
    route.end = static_cast<std::int16_t>(current);
    return route;
}

GameOutput decide(const GameInput& input) noexcept {
    beginTurn(input);
    const bool slowHand = likelySlowHand(input);
    int ground[kCells];
    std::memcpy(ground, input.grid, sizeof(ground));
    std::uint32_t forbidden[kWords];
    std::uint32_t preempted[kWords];
    prepareBoard(input, forbidden, slowHand, preempted);
    updateSnapshotTarget(input);

    LocalScan scans[2]{};
    scanLocal(input, 0, forbidden, ground, slowHand, preempted, scans[0]);
    scanLocal(input, 1, forbidden, ground, slowHand, preempted, scans[1]);
    for (int word = 0; word < kWords; ++word) {
        forbidden[word] |= gState.bombs[word];
    }
    const int allocation = chooseAllocation(scans[0], scans[1]);
    const int budgets[2] = {allocation, S - allocation};
    const int firstUnit =
        allocation == 0
            ? 1
            : allocation == S
                  ? 0
                  : (scans[1].projected[budgets[1] - 1] >
                             scans[0].projected[budgets[0] - 1]
                         ? 1
                         : 0);
    const int secondUnit = 1 - firstUnit;
    GoldState goldState{{}, {}, 0, 0};
    Target targets[2] = {selectTarget(input.my_units[0], scans[0],
                                      budgets[0], ground, -1, goldState),
                         selectTarget(input.my_units[1], scans[1],
                                      budgets[1], ground, -1, goldState)};
    const Target macros[2] = {macroTarget(input, 0), macroTarget(input, 1)};

    Route routes[2]{};
    const int secondStart = cellOf(input.my_units[secondUnit].row,
                                   input.my_units[secondUnit].col);
    routes[firstUnit] = makeRoute(input, firstUnit, targets[firstUnit],
                                  macros[firstUnit], secondStart,
                                  budgets[firstUnit], forbidden, ground,
                                  goldState);
    int secondTargetGold = clampGold(ground[targets[secondUnit].cell]);
    for (int changed = 0; changed < goldState.count; ++changed) {
        if (goldState.cells[changed] == targets[secondUnit].cell) {
            secondTargetGold = goldState.values[changed];
            break;
        }
    }
    if (targets[secondUnit].cell == routes[firstUnit].end ||
        secondTargetGold <= 0) {
        targets[secondUnit] = selectTarget(
            input.my_units[secondUnit], scans[secondUnit],
            budgets[secondUnit], ground, routes[firstUnit].end, goldState);
    }
    routes[secondUnit] = makeRoute(
        input, secondUnit, targets[secondUnit], macros[secondUnit],
        routes[firstUnit].end, budgets[secondUnit], forbidden, ground,
        goldState);

    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay},
                      allocation, firstUnit, 0};
    for (int step = 0; step < budgets[0]; ++step) {
        output.actions[step] = routes[0].actions[step];
    }
    for (int step = 0; step < budgets[1]; ++step) {
        output.actions[allocation + step] = routes[1].actions[step];
    }
    rememberExpectedGain(input, goldState.expectedGain);
    return output;
}

}  // namespace

extern "C" __attribute__((visibility("default")))
GameOutput moveDecision(const GameInput* input) {
    if (input == nullptr || input->round < 0 || input->round > 1000000 ||
        !validPosition(input->my_units[0]) ||
        !validPosition(input->my_units[1]) ||
        (input->my_units[0].row == input->my_units[1].row &&
         input->my_units[0].col == input->my_units[1].col)) {
        return fallbackOutput();
    }
    return decide(*input);
}
