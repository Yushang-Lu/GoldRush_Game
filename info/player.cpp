#include "game_api.h"

#include <climits>
#include <cstdint>
#include <cstring>

static_assert(sizeof(int) == 4, "比赛 ABI 要求 int 为4字节");
static_assert(sizeof(Position) == 8, "Position ABI 布局不匹配");
static_assert(sizeof(NpcInfo) == 12, "NpcInfo ABI 布局不匹配");
static_assert(sizeof(RegionStat) == 28, "RegionStat ABI 布局不匹配");
static_assert(sizeof(Snapshot) == 148, "Snapshot ABI 布局不匹配");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI 布局不匹配");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI 布局不匹配");
static_assert(MAX_NPCS <= 7, "NPC计数必须能放入三位标记");

namespace {

constexpr int kRows = 17;
constexpr int kCells = 289;
constexpr int kStay = 4;
constexpr int kDr[5] = {-1, 1, 0, 0, 0};
constexpr int kDc[5] = {0, 0, -1, 1, 0};
constexpr unsigned char kNpcCountMask = 0x07U;
constexpr unsigned char kStationaryEnemyFlag = 0x08U;
constexpr unsigned char kPreemptedFlag = 0x10U;
[[maybe_unused]] constexpr unsigned char kPredictedEnemyFlag = 0x20U;
#if defined(GOLD_ABLATE_ENEMY_PREDICTOR)
constexpr unsigned char kGoldPreemptedMask = kPreemptedFlag;
#else
constexpr unsigned char kGoldPreemptedMask =
    kPreemptedFlag | kPredictedEnemyFlag;
#endif
constexpr uint64_t kReachLaneEven = 0x5555555555555555ULL;
constexpr uint64_t kReachLaneOdd = 0xAAAAAAAAAAAAAAAAULL;

struct LocalOffset {
    signed char dr;
    signed char dc;
    signed char cellDelta;
    unsigned char distance;
};

// 5x5 邻域，行优先，附带曼哈顿距离，避免热点循环里反复取绝对值。
static constexpr LocalOffset kLocal25[25] = {
    {-2, -2, -36, 4}, {-2, -1, -35, 3}, {-2, 0, -34, 2}, {-2, 1, -33, 3},
    {-2, 2, -32, 4},
    {-1, -2, -19, 3}, {-1, -1, -18, 2}, {-1, 0, -17, 1}, {-1, 1, -16, 2},
    {-1, 2, -15, 3},
    {0, -2, -2, 2},  {0, -1, -1, 1},   {0, 0, 0, 0},    {0, 1, 1, 1},
    {0, 2, 2, 2},
    {1, -2, 15, 3},  {1, -1, 16, 2},   {1, 0, 17, 1},   {1, 1, 18, 2},
    {1, 2, 19, 3},
    {2, -2, 32, 4},  {2, -1, 33, 3},   {2, 0, 34, 2},   {2, 1, 35, 3},
    {2, 2, 36, 4},
};

inline int absInt(int value) noexcept { return value < 0 ? -value : value; }

inline bool inside(int row, int col) noexcept {
    return static_cast<unsigned>(row) < 17U &&
           static_cast<unsigned>(col) < 17U;
}

inline int cellOf(int row, int col) noexcept { return row * kRows + col; }

inline int remainingGold(int gold) noexcept {
    return gold > 0 ? static_cast<int>((7LL * gold) / 20) : 0;
}

inline int pickupGold(int gold) noexcept {
    // 对正金币直接计算 ceil(13g/20)；非正值保持原先 gold-0 的语义。
    return gold > 0 ? static_cast<int>((13LL * gold + 19) / 20) : gold;
}

inline int saturatingGainAdd(int current, int gain) noexcept {
    return current > INT_MAX - gain ? INT_MAX : current + gain;
}

inline int effectiveGold(int gold, unsigned char npcInfo) noexcept {
    return gold > 0 && (npcInfo & kGoldPreemptedMask) != 0U
               ? remainingGold(gold)
               : gold;
}

struct Target {
    int row;
    int col;
    int gold;
};

struct LocalCandidate {
    int estimatedGold;
    int prefixTake[3];
    unsigned short cell;
    unsigned char distance;
    unsigned char padding;
};

static_assert(sizeof(LocalCandidate) == 20,
              "局部金币候选应保持紧凑布局");

struct LocalGoldScan {
    LocalCandidate candidates[25];
    long long opportunity = 0;
    int projectedTake[5]{};
    int count = 0;
};

struct Route {
    unsigned char actions[5]{kStay, kStay, kStay, kStay, kStay};
    short endCell = -1;
    int expectedGain = 0;
};

static_assert(sizeof(Route) == 12, "极速路径应保持双寄存器返回布局");

struct SpeedBelief {
    int round = -1;
    long long held = 0;
    long long expectedGain = 0;
    int slowEvidence = 0;
};

struct EnemyPositionMemory {
    int round = -2;
    short cells[5][2]{};
};

struct FailureGuard {
    short originCell;
    bool active;
};

struct ThreadState {
    SpeedBelief speed;
    EnemyPositionMemory enemies;
};

thread_local ThreadState threadState;

[[maybe_unused]] void rememberEnemyPositions(
    EnemyPositionMemory& memory, const GameInput& input) noexcept {
    if (input.round < 0) {
        memory = EnemyPositionMemory{};
        return;
    }
    if (input.round <= memory.round ||
        static_cast<long long>(input.round) !=
            static_cast<long long>(memory.round) + 1LL) {
        for (auto& roundCells : memory.cells) {
            roundCells[0] = -1;
            roundCells[1] = -1;
        }
    }
    short* const current = memory.cells[input.round % 5];
    current[0] = -1;
    current[1] = -1;
    int count = 0;
    for (const Position enemy : input.visible_enemies) {
        if (inside(enemy.row, enemy.col) && count < 2) {
            current[count++] = static_cast<short>(cellOf(enemy.row, enemy.col));
        }
    }
    memory.round = input.round;
}

bool plausibleEnemyBlock(const GameInput& input, int targetCell,
                         [[maybe_unused]] const unsigned char
                             npcCounts[kCells],
                         const EnemyPositionMemory& memory) noexcept {
    // 失败分支只在有可观测阻挡证据时启用，避免把所有普通移动都当作
    // 可能失败而牺牲采金：本轮预测终点、最近两轮三步可达位置，或
    // 最近五轮曾被敌方精确占据的格子。
#if !defined(GOLD_ABLATE_ENEMY_PREDICTOR)
    if ((npcCounts[targetCell] & kPredictedEnemyFlag) != 0U) return true;
#endif

    const int targetRow = targetCell / kRows;
    const int targetCol = targetCell - targetRow * kRows;
    for (int age = 0; age <= 1 && age <= input.round; ++age) {
        const int slot = (input.round - age) % 5;
        for (const short cell : memory.cells[slot]) {
            if (cell < 0) continue;
            const int row = cell / kRows;
            const int col = cell - row * kRows;
            if (absInt(targetRow - row) + absInt(targetCol - col) <= 3) {
                return true;
            }
        }
    }
    for (const auto& roundCells : memory.cells) {
        if (roundCells[0] == targetCell || roundCells[1] == targetCell) {
            return true;
        }
    }
    return false;
}

bool enemyCanVacate(const GameInput& input, Position enemy) noexcept {
    const int own0 = cellOf(input.my_units[0].row, input.my_units[0].col);
    const int own1 = cellOf(input.my_units[1].row, input.my_units[1].col);
    for (int action = 0; action < 4; ++action) {
        const int row = enemy.row + kDr[action];
        const int col = enemy.col + kDc[action];
        if (!inside(row, col)) continue;
        const int cell = cellOf(row, col);
        const int tile = input.grid[row][col];
        if (tile != -1 && tile != -3 && cell != own0 && cell != own1) {
            return true;
        }
    }
    return false;
}

inline void markPreemptedNeighbors(unsigned char npcCounts[kCells],
                                   int row, int col) noexcept {
    const int cell = cellOf(row, col);
    if (row > 0) npcCounts[cell - 17] |= kPreemptedFlag;
    if (row < 16) npcCounts[cell + 17] |= kPreemptedFlag;
    if (col > 0) npcCounts[cell - 1] |= kPreemptedFlag;
    if (col < 16) npcCounts[cell + 1] |= kPreemptedFlag;
    npcCounts[cell] |= kPreemptedFlag;
}

// 三步可达计数以 17 行 x 17 列的 2 位饱和计数（0..3）打包成 17 个 uint64，
// 整行一次完成，避免逐格递增的 49xNPC 标量开销。0xc0 的旧语义即计数 3。
inline uint64_t reachSpanMask(int left, int right) noexcept {
    const uint64_t bits = (1ULL << (2 * (right + 1))) -
                          (1ULL << (2 * left));
    return bits & kReachLaneEven;
}

inline void markReachRow(uint64_t& word, uint64_t mask) noexcept {
    const uint64_t oldLo = word & kReachLaneEven;
    const uint64_t oldHi = word & kReachLaneOdd;
    const uint64_t newLo =
        (oldLo & ~mask) | (mask & ((oldHi >> 1) | ~word));
    const uint64_t newHi =
        (oldHi & ~(mask << 1)) | ((mask << 1) & (word | (oldLo << 1)));
    word = newLo | newHi;
}

inline void markNpcThreeStepReach(uint64_t reachRows[17],
                                  int npcRow, int npcCol) noexcept {
    for (int dr = -3; dr <= 3; ++dr) {
        const int row = npcRow + dr;
        if (static_cast<unsigned>(row) >= 17U) continue;
        const int horizontal = 3 - (dr < 0 ? -dr : dr);
        int left = npcCol - horizontal;
        if (left < 0) left = 0;
        int right = npcCol + horizontal;
        if (right > 16) right = 16;
        markReachRow(reachRows[row], reachSpanMask(left, right));
    }
}

inline bool reachCellCrowded(const uint64_t reachRows[17],
                             int row, int col) noexcept {
    return ((reachRows[row] >> (2 * col)) & 3U) == 3U;
}

[[maybe_unused]] inline bool entersNpcReachCrowd(
    int row, int col, int action, bool protectNpcReach,
    const uint64_t reachRows[17]) noexcept {
    if (!protectNpcReach || action == kStay ||
        reachCellCrowded(reachRows, row, col)) {
        return false;
    }
    const int nextRow = row + kDr[action];
    const int nextCol = col + kDc[action];
    return inside(nextRow, nextCol) &&
           reachCellCrowded(reachRows, nextRow, nextCol);
}

#if !defined(GOLD_ABLATE_ENEMY_PREDICTOR)
int predictEnemyEndpoint(const GameInput& input, Position enemy) noexcept {
    Target target{enemy.row, enemy.col, 0};
    int bestTake = 0;
    int bestDistance = 0;
    for (int index = 0; index < 25; ++index) {
        const int row = enemy.row + kLocal25[index].dr;
        if (static_cast<unsigned>(row) >= 17U) continue;
        const int col = enemy.col + kLocal25[index].dc;
        if (static_cast<unsigned>(col) >= 17U) continue;
        const int gold = input.grid[row][col];
        if (gold <= 0) continue;
        const int take = pickupGold(gold);
        const int distance = kLocal25[index].distance;
        if (take > bestTake ||
            (take == bestTake && distance < bestDistance)) {
            target = Target{row, col, gold};
            bestTake = take;
            bestDistance = distance;
        }
    }
    if (bestTake == 0) return -1;

    int row = enemy.row;
    int col = enemy.col;
    const int own0 = cellOf(input.my_units[0].row, input.my_units[0].col);
    const int own1 = cellOf(input.my_units[1].row, input.my_units[1].col);
    for (int step = 0; step < 3 && (row != target.row || col != target.col);
         ++step) {
        const int rowGap = absInt(target.row - row);
        const int colGap = absInt(target.col - col);
        const bool verticalFirst = rowGap >= colGap;
        int action = verticalFirst
                         ? (target.row < row ? 0 : 1)
                         : (target.col < col ? 2 : 3);
        const int nextRow = row + kDr[action];
        const int nextCol = col + kDc[action];
        const int nextCell = nextRow * kRows + nextCol;
        bool moved = inside(nextRow, nextCol) &&
                     input.grid[nextRow][nextCol] != -1 &&
                     input.grid[nextRow][nextCol] != -3 &&
                     nextCell != own0 && nextCell != own1;
        if (!moved &&
            (verticalFirst ? colGap > 0 : rowGap > 0)) {
            const int second = verticalFirst
                                   ? (target.col < col ? 2 : 3)
                                   : (target.row < row ? 0 : 1);
            const int secondRow = row + kDr[second];
            const int secondCol = col + kDc[second];
            const int secondCell = secondRow * kRows + secondCol;
            if (inside(secondRow, secondCol) &&
                input.grid[secondRow][secondCol] != -1 &&
                input.grid[secondRow][secondCol] != -3 &&
                secondCell != own0 && secondCell != own1) {
                action = second;
                moved = true;
            }
        }
        if (!moved) break;
        row += kDr[action];
        col += kDc[action];
    }
    return cellOf(row, col);
}
#endif

Target macroWaypoint(const GameInput& input, int unit) noexcept {
    const Position start = input.my_units[unit];
    if (start.row >= 5 && start.row <= 11 && start.col >= 5 && start.col <= 11) {
        static constexpr int patrol[8][2] = {
            {6, 7}, {7, 10}, {9, 10}, {10, 9},
            {10, 7}, {9, 6}, {7, 6}, {6, 9}
        };
        const int phase = ((input.round >> 2) + unit * 4) & 7;
        return Target{patrol[phase][0], patrol[phase][1], 0};
    }
    return unit == 0 ? Target{7, 8, 0} : Target{9, 8, 0};
}

inline bool hasStaticEntry(const int ground[kCells], int row,
                           int col) noexcept {
    const int cell = cellOf(row, col);
    if (row > 0) {
        const int value = ground[cell - 17];
        if (value != -1 && value != -3) return true;
    }
    if (row < 16) {
        const int value = ground[cell + 17];
        if (value != -1 && value != -3) return true;
    }
    if (col > 0) {
        const int value = ground[cell - 1];
        if (value != -1 && value != -3) return true;
    }
    if (col < 16) {
        const int value = ground[cell + 1];
        if (value != -1 && value != -3) return true;
    }
    return false;
}

void buildLegalTables(const int ground[kCells],
                      const unsigned char npcCounts[kCells], bool robust,
                      const int enemyCells[2],
                      unsigned char legalLow[kCells],
                      unsigned char legalHigh[kCells]) noexcept {
    const unsigned int stationaryMask =
        robust ? kStationaryEnemyFlag : 0U;
    for (int cell = 0; cell < kCells; ++cell) {
        const int value = ground[cell];
        const unsigned int info = npcCounts[cell];
        const unsigned int open =
            static_cast<unsigned int>(value != -1) &
            static_cast<unsigned int>(value != -3) &
            static_cast<unsigned int>((info & kNpcCountMask) < 3U) &
            static_cast<unsigned int>((info & stationaryMask) == 0U);
        legalLow[cell] = static_cast<unsigned char>(open);
        legalHigh[cell] = static_cast<unsigned char>(
            open & static_cast<unsigned int>(value != -5));
    }
    if (!robust) {
        if (enemyCells[0] >= 0) {
            legalLow[enemyCells[0]] = 0;
            legalHigh[enemyCells[0]] = 0;
        }
        if (enemyCells[1] >= 0) {
            legalLow[enemyCells[1]] = 0;
            legalHigh[enemyCells[1]] = 0;
        }
    }
}

inline bool hasSafeExit(const Position start,
                        const unsigned char legal[kCells]) noexcept {
    for (int action = 0; action < 4; ++action) {
        const int row = start.row + kDr[action];
        const int col = start.col + kDc[action];
        if (inside(row, col) && legal[cellOf(row, col)] != 0U) {
            return true;
        }
    }
    return false;
}

void scanLocalGold(const GameInput& input, int unit,
                   const unsigned char npcCounts[kCells],
                   const int ground[kCells],
                   const unsigned char legalLow[kCells],
                   const unsigned char legalHigh[kCells],
                   LocalGoldScan& scan) noexcept {
    const Position start = input.my_units[unit];
    const int startCell = cellOf(start.row, start.col);
    scan.opportunity = 0;
    scan.count = 0;
    for (int budget = 0; budget < 5; ++budget) {
        scan.projectedTake[budget] = 0;
    }
    const unsigned char* const exitLegal =
        input.my_units_gold[unit] < 200 ? legalLow : legalHigh;
    if (!hasSafeExit(start, exitLegal)) {
        return;
    }
    for (int index = 0; index < 25; ++index) {
        const int row = start.row + kLocal25[index].dr;
        if (static_cast<unsigned>(row) >= 17U) continue;
        const int col = start.col + kLocal25[index].dc;
        if (static_cast<unsigned>(col) >= 17U) continue;
        const int cell = startCell + kLocal25[index].cellDelta;
        const int gold = ground[cell];
        const unsigned char info = npcCounts[cell];
        if (__builtin_expect(gold <= 0, 1) ||
            __builtin_expect(legalLow[cell] == 0U, 0)) {
            continue;
        }
        const int distance = kLocal25[index].distance;
        if (distance != 0 && !hasStaticEntry(ground, row, col)) continue;
        const int estimatedGold = effectiveGold(gold, info);
        const int remaining1 = remainingGold(estimatedGold);
        const int take1 = estimatedGold - remaining1;
        scan.opportunity += take1;
        int prefix2 = take1;
        int prefix3 = take1;
        if (distance != 4) {
            const int remaining2 = remainingGold(remaining1);
            const int take2 = remaining1 - remaining2;
            prefix2 = take1 + take2;
            prefix3 = prefix2;
            if (distance == 1) {
                const int remaining3 = remainingGold(remaining2);
                const int take3 = remaining2 - remaining3;
                prefix3 = prefix2 + take3;
            }
        }
        switch (distance) {
        case 0:
        case 2:
            if (take1 > scan.projectedTake[1]) scan.projectedTake[1] = take1;
            if (take1 > scan.projectedTake[2]) scan.projectedTake[2] = take1;
            if (prefix2 > scan.projectedTake[3]) {
                scan.projectedTake[3] = prefix2;
            }
            if (prefix2 > scan.projectedTake[4]) {
                scan.projectedTake[4] = prefix2;
            }
            break;
        case 1:
            if (take1 > scan.projectedTake[0]) scan.projectedTake[0] = take1;
            if (take1 > scan.projectedTake[1]) scan.projectedTake[1] = take1;
            if (prefix2 > scan.projectedTake[2]) {
                scan.projectedTake[2] = prefix2;
            }
            if (prefix2 > scan.projectedTake[3]) {
                scan.projectedTake[3] = prefix2;
            }
            if (prefix3 > scan.projectedTake[4]) {
                scan.projectedTake[4] = prefix3;
            }
            break;
        case 3:
            if (take1 > scan.projectedTake[2]) scan.projectedTake[2] = take1;
            if (take1 > scan.projectedTake[3]) scan.projectedTake[3] = take1;
            if (prefix2 > scan.projectedTake[4]) {
                scan.projectedTake[4] = prefix2;
            }
            break;
        default:
            if (take1 > scan.projectedTake[3]) {
                scan.projectedTake[3] = take1;
            }
            if (take1 > scan.projectedTake[4]) {
                scan.projectedTake[4] = take1;
            }
            break;
        }
        scan.candidates[scan.count++] = LocalCandidate{
            estimatedGold,
            {take1, prefix2, prefix3},
            static_cast<unsigned short>(cell),
            static_cast<unsigned char>(distance), 0};
    }
}

template <bool refreshFromGround>
Target selectGoldWaypoint(const Position start, const LocalGoldScan& scan,
                          const unsigned char npcCounts[kCells],
                          const int ground[kCells], int actionBudget,
                          int forbiddenCell = -1) noexcept {
    Target best{start.row, start.col, 0};
    Target approach{start.row, start.col, 0};
    int bestProjectedTake = 0;
    int bestFirstTake = 0;
    int bestDistance = 0;
    int approachTake = 0;
    for (int index = 0; index < scan.count; ++index) {
        const LocalCandidate candidate = scan.candidates[index];
        const int cell = candidate.cell;
        if (cell == forbiddenCell) continue;
        const int distance = candidate.distance;
        const int estimatedGold = refreshFromGround
                                      ? effectiveGold(ground[cell],
                                                      npcCounts[cell])
                                      : candidate.estimatedGold;
        if (estimatedGold <= 0) continue;
        const int amount = refreshFromGround ? pickupGold(estimatedGold)
                                             : candidate.prefixTake[0];
        if (distance > actionBudget + 1) continue;
        if (distance == actionBudget + 1 ||
            (distance == 0 && actionBudget < 2)) {
            if (amount > approachTake) {
                const int row = cell / kRows;
                const int col = cell - row * kRows;
                approach = Target{row, col, estimatedGold};
                approachTake = amount;
            }
            continue;
        }

        const int entries = distance == 0
                                ? actionBudget / 2
                                : 1 + (actionBudget - distance) / 2;
        int projectedTake;
        if constexpr (refreshFromGround) {
            if (entries == 1) {
                projectedTake = amount;
            } else if (entries == 2) {
                projectedTake = amount + pickupGold(estimatedGold - amount);
            } else {
                const int remaining1 = estimatedGold - amount;
                const int take2 = pickupGold(remaining1);
                projectedTake =
                    amount + take2 + pickupGold(remaining1 - take2);
            }
        } else {
            projectedTake = candidate.prefixTake[entries - 1];
        }
        if (projectedTake > bestProjectedTake ||
            (projectedTake == bestProjectedTake &&
             (amount > bestFirstTake ||
              (amount == bestFirstTake && distance < bestDistance)))) {
            const int row = cell / kRows;
            const int col = cell - row * kRows;
            best = Target{row, col, estimatedGold};
            bestProjectedTake = projectedTake;
            bestFirstTake = amount;
            bestDistance = distance;
        }
    }
    return bestProjectedTake > 0 ? best : approach;
}

inline bool legalStep(int row, int col, int blockedCell, int reservedCell,
                      const unsigned char legal[kCells]) noexcept {
    if (!inside(row, col)) return false;
    const int cell = cellOf(row, col);
    return legal[cell] != 0U && cell != blockedCell && cell != reservedCell;
}

inline int applyStep(int action, int& row, int& col,
                     const unsigned char npcCounts[kCells],
                     int ground[kCells]) noexcept {
    row += kDr[action];
    col += kDc[action];
    const int cell = cellOf(row, col);
    if (ground[cell] <= 0) return 0;
    // ground 保存行动前原始金币序列；预占标记相当于快方已经先吃一口。
    // 每次我方进入仍按原始序列推进一次，effectiveGold 再统一偏移一口，
    // 从而让反馈学习使用的 expectedGain 与慢手估值保持同一口径。
    const int take = pickupGold(effectiveGold(ground[cell], npcCounts[cell]));
    ground[cell] = remainingGold(ground[cell]);
    return take;
}

bool safeForFailureGuard(
    int action, const FailureGuard& guard,
    const int ground[kCells]) noexcept {
    if (action == kStay || !guard.active) return true;
    const int row = guard.originCell / kRows;
    const int col = guard.originCell - row * kRows;
    const int nextRow = row + kDr[action];
    const int nextCol = col + kDc[action];
    // 越界、墙体和再次碰撞只会取消这一步，不会直接损失金币；
    // 这里仅保护“上一步失败后，紧随动作从原位踩入已知炸弹”。
    if (!inside(nextRow, nextCol)) return true;
    return ground[cellOf(nextRow, nextCol)] != -3;
}

inline bool hasAdjacentBomb(int row, int col,
                            const int ground[kCells]) noexcept {
    for (int action = 0; action < 4; ++action) {
        const int nextRow = row + kDr[action];
        const int nextCol = col + kDc[action];
        if (inside(nextRow, nextCol) &&
            ground[cellOf(nextRow, nextCol)] == -3) {
            return true;
        }
    }
    return false;
}

inline void replaceFailureGuard(int action, int nominalRow, int nominalCol,
                                bool enabled, const GameInput& input,
                                const unsigned char npcCounts[kCells],
                                const int ground[kCells],
                                const EnemyPositionMemory& memory,
                                FailureGuard& guard) noexcept {
    guard.active = false;
    if (!enabled || action == kStay) return;
    const int nextRow = nominalRow + kDr[action];
    const int nextCol = nominalCol + kDc[action];
    if (!inside(nextRow, nextCol)) return;
    if (!hasAdjacentBomb(nominalRow, nominalCol, ground) ||
        !plausibleEnemyBlock(input, cellOf(nextRow, nextCol), npcCounts,
                             memory)) {
        return;
    }
    guard.originCell = static_cast<short>(cellOf(nominalRow, nominalCol));
    guard.active = true;
}

int chooseTowardAction(int row, int col, const Target target,
                       int previousAction, int blockedCell, int reservedCell,
                       [[maybe_unused]] bool protectNpcReach,
                       const FailureGuard& guard,
                       [[maybe_unused]] const uint64_t reachRows[17],
                       const unsigned char npcCounts[kCells],
                       const int ground[kCells],
                       const unsigned char legal[kCells]) noexcept {
    int bestAction = kStay;
    long long bestScore = -0x3fffffffLL;
    const int rowGap = absInt(target.row - row);
    const int colGap = absInt(target.col - col);
    for (int action = 0; action < 4; ++action) {
        const int nextRow = row + kDr[action];
        const int nextCol = col + kDc[action];
#if !defined(GOLD_ABLATE_NPC_REACH)
        if (entersNpcReachCrowd(
                row, col, action, protectNpcReach, reachRows)) {
            continue;
        }
#endif
        if (!legalStep(nextRow, nextCol, blockedCell, reservedCell, legal)) {
            continue;
        }
        if (!safeForFailureGuard(action, guard, ground)) {
            continue;
        }
        const int nextCell = cellOf(nextRow, nextCol);
        const int value = effectiveGold(ground[nextCell], npcCounts[nextCell]);
        const int distance =
            absInt(target.row - nextRow) + absInt(target.col - nextCol);
        long long score = -static_cast<long long>(distance) * 256;
        if (value > 0) {
            score += static_cast<long long>(pickupGold(value)) * 4096;
        }
#if !defined(GOLD_ABLATE_ENEMY_PREDICTOR)
        if (ground[nextCell] <= 0 &&
            (npcCounts[nextCell] & kPredictedEnemyFlag) != 0U) {
            // 有金币终点已通过effectiveGold折价；这里只轻避没有收益、
            // 却可能成为快方最终落点的空格。
            score -= 4096;
        }
#endif
        const bool vertical = action < 2;
        if ((vertical && rowGap > colGap) || (!vertical && colGap > rowGap)) {
            score += 32;
        }
        if (previousAction >= 0 &&
            ((previousAction == 0 && action == 1) ||
             (previousAction == 1 && action == 0) ||
             (previousAction == 2 && action == 3) ||
             (previousAction == 3 && action == 2))) {
            score -= 8;
        }
        if (score > bestScore) {
            bestScore = score;
            bestAction = action;
        }
    }
    return bestAction;
}

int chooseBounceAction(int row, int col, const Target macro,
                       int blockedCell, int reservedCell,
                       [[maybe_unused]] bool protectNpcReach,
                       const FailureGuard& guard,
                       [[maybe_unused]] const uint64_t reachRows[17],
                       const unsigned char npcCounts[kCells],
                       const int ground[kCells],
                       const unsigned char legal[kCells]) noexcept {
    int bestAction = kStay;
    long long bestScore = -0x3fffffffLL;
    for (int action = 0; action < 4; ++action) {
        const int nextRow = row + kDr[action];
        const int nextCol = col + kDc[action];
#if !defined(GOLD_ABLATE_NPC_REACH)
        if (entersNpcReachCrowd(
                row, col, action, protectNpcReach, reachRows)) {
            continue;
        }
#endif
        if (!legalStep(nextRow, nextCol, blockedCell, reservedCell, legal)) {
            continue;
        }
        if (!safeForFailureGuard(action, guard, ground)) {
            continue;
        }
        const int nextCell = cellOf(nextRow, nextCol);
        const int value = effectiveGold(ground[nextCell], npcCounts[nextCell]);
        const int distance =
            absInt(macro.row - nextRow) + absInt(macro.col - nextCol);
        long long score = -static_cast<long long>(distance) * 32;
        if (value > 0) {
            score += static_cast<long long>(pickupGold(value)) * 4096;
        }
#if !defined(GOLD_ABLATE_ENEMY_PREDICTOR)
        if (ground[nextCell] <= 0 &&
            (npcCounts[nextCell] & kPredictedEnemyFlag) != 0U) {
            // 空终点没有资源补偿时，用一枚金币等价打破路线平局。
            score -= 4096;
        }
#endif
        if (score > bestScore) {
            bestScore = score;
            bestAction = action;
        }
    }
    return bestAction;
}

bool npcPackEmergency(const GameInput& input, int unit, bool robust,
                      const unsigned char npcCounts[kCells]) noexcept {
    const Position start = input.my_units[unit];
    if (!robust || input.my_units_gold[unit] < 500 ||
        (npcCounts[cellOf(start.row, start.col)] & kNpcCountMask) < 2U) {
        return false;
    }

    int npcLimit = input.num_visible_npcs;
    if (npcLimit < 0) npcLimit = 0;
    if (npcLimit > MAX_NPCS) npcLimit = MAX_NPCS;
    int reachableNpc = 0;
    for (int index = 0; index < npcLimit; ++index) {
        const Position npc = input.visible_npcs[index].pos;
        if (inside(npc.row, npc.col) &&
            absInt(npc.row - start.row) + absInt(npc.col - start.col) <= 3) {
            ++reachableNpc;
        }
    }
    return reachableNpc >= 3;
}

Route makeRoute(const GameInput& input, int unit, const Target target,
                const Target macro, int blockedCell, int reservedCell,
                const uint64_t reachRows[17],
                const unsigned char npcCounts[kCells], int ground[kCells],
                const unsigned char legalLow[kCells],
                const unsigned char legalHigh[kCells],
                int actionCount, [[maybe_unused]] bool robust,
                bool emergencyStay,
                const EnemyPositionMemory& memory) noexcept {
    Route route;
    int row = input.my_units[unit].row;
    int col = input.my_units[unit].col;
    int previousAction = -1;
    const int held = input.my_units_gold[unit];
    const unsigned char* const legal = held < 200 ? legalLow : legalHigh;
    const bool guardEnabled =
#if defined(GOLD_ABLATE_FAILURE_GUARD)
        false;
#else
        robust && held >= 250;
#endif
    const bool protectNpcReach =
#if defined(GOLD_ABLATE_NPC_REACH)
        false;
#else
        robust && held >= 600;
#endif
    FailureGuard guard{};

    // 三只以上可见 NPC 已经包围高持币慢手角色时，原地不动不会触发
    // 踩踏；预算层只给该角色保留一步，其余动作让给安全队友。
    if (emergencyStay) {
        route.endCell = static_cast<short>(cellOf(row, col));
        return route;
    }

    for (int step = 0; step < actionCount; ++step) {
        int action = kStay;
        if (target.gold > 0 && row == target.row && col == target.col) {
            const int bounce = chooseBounceAction(
                row, col, macro, blockedCell, reservedCell, protectNpcReach,
                guard, reachRows, npcCounts, ground, legal);
            if (bounce != kStay) {
                route.actions[step] = static_cast<unsigned char>(bounce);
                replaceFailureGuard(bounce, row, col, guardEnabled, input,
                                    npcCounts, ground, memory, guard);
                route.expectedGain = saturatingGainAdd(
                    route.expectedGain,
                    applyStep(bounce, row, col, npcCounts, ground));
                previousAction = bounce;
                if (step + 1 < actionCount) {
                    const int reverse = bounce ^ 1;
                    if (
#if !defined(GOLD_ABLATE_NPC_REACH)
                        !entersNpcReachCrowd(
                            row, col, reverse, protectNpcReach, reachRows) &&
#endif
                        safeForFailureGuard(reverse, guard, ground)) {
                        ++step;
                        route.actions[step] =
                            static_cast<unsigned char>(reverse);
                        replaceFailureGuard(reverse, row, col, guardEnabled,
                                            input, npcCounts, ground, memory,
                                            guard);
                        route.expectedGain = saturatingGainAdd(
                            route.expectedGain,
                            applyStep(reverse, row, col, npcCounts, ground));
                        previousAction = reverse;
                    }
                }
                continue;
            }
        } else {
            action = chooseTowardAction(
                row, col, target, previousAction, blockedCell, reservedCell,
                protectNpcReach, guard, reachRows, npcCounts, ground, legal);
        }

        route.actions[step] = static_cast<unsigned char>(action);
        replaceFailureGuard(action, row, col, guardEnabled, input, npcCounts,
                            ground, memory, guard);
        if (action != kStay) {
            route.expectedGain = saturatingGainAdd(
                route.expectedGain,
                applyStep(action, row, col, npcCounts, ground));
            previousAction = action;
        }
    }
    route.endCell = static_cast<short>(cellOf(row, col));
    return route;
}

int proportionalAllocation(long long opportunity0,
                           long long opportunity1) noexcept {
    if (opportunity0 > opportunity1) {
        return opportunity0 > 2 * opportunity1 ? 5 : 4;
    }
    if (opportunity1 > opportunity0) {
        return opportunity1 > 2 * opportunity0 ? 1 : 2;
    }
    return 3;
}

int chooseAllocation(const LocalGoldScan& scan0,
                     const LocalGoldScan& scan1) noexcept {
    const int proportional =
        proportionalAllocation(scan0.opportunity, scan1.opportunity);
    int bestAllocation = proportional;
    int bestDistance = 0;
    long long bestValue = scan0.projectedTake[proportional - 1] +
                          static_cast<long long>(
                              scan1.projectedTake[5 - proportional]);
    for (int allocation = 1; allocation <= 5; ++allocation) {
        const long long value = scan0.projectedTake[allocation - 1] +
                                static_cast<long long>(
                                    scan1.projectedTake[5 - allocation]);
        const int distance = absInt(allocation - proportional);
        if (value > bestValue ||
            (value == bestValue && distance < bestDistance)) {
            bestValue = value;
            bestAllocation = allocation;
            bestDistance = distance;
        }
    }
    return bestAllocation;
}

GameOutput safeOutput() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 3, 0, 0};
}

bool likelySlowHand(SpeedBelief& belief, const GameInput& input) noexcept {
    const long long held =
        static_cast<long long>(input.my_units_gold[0]) +
        input.my_units_gold[1];
    if (input.round == 0 || belief.round < 0 ||
        static_cast<long long>(input.round) !=
            static_cast<long long>(belief.round) + 1LL) {
        belief = SpeedBelief{};
        belief.held = held;
        return false;
    }

    if (belief.expectedGain >= 2) {
        const long long actualGain = held - belief.held;
        // 风险扣款会令持币变化为负，不能把它误判成“资源被快手抢先”。
        // 只累计非负但低于可见路径预期的独立缺口，连续两次才启用折价。
        if (actualGain >= 0 && actualGain < belief.expectedGain) {
            ++belief.slowEvidence;
            if (belief.slowEvidence > 8) belief.slowEvidence = 8;
        } else if (actualGain >= belief.expectedGain &&
                   belief.slowEvidence > 0 && belief.slowEvidence < 2) {
            --belief.slowEvidence;
        }
    }
    return belief.slowEvidence >= 2;
}

void rememberExpectedGain(SpeedBelief& belief, const GameInput& input,
                          long long expectedGain) noexcept {
    belief.round = input.round;
    belief.held = static_cast<long long>(input.my_units_gold[0]) +
                  input.my_units_gold[1];
    belief.expectedGain = expectedGain;
}

GameOutput decide(const GameInput& input) noexcept {
    if (!inside(input.my_units[0].row, input.my_units[0].col) ||
        !inside(input.my_units[1].row, input.my_units[1].col)) {
        return safeOutput();
    }

    ThreadState& state = threadState;
#if !defined(GOLD_ABLATE_FAILURE_GUARD)
    rememberEnemyPositions(state.enemies, input);
#endif
    int ground[kCells];
    std::memcpy(ground, input.grid, sizeof(ground));
    unsigned char npcCounts[kCells]{};
    uint64_t reachRows[17]{};
    const Position enemy0 = input.visible_enemies[0];
    const Position enemy1 = input.visible_enemies[1];
    const int enemyCells[2] = {
        inside(enemy0.row, enemy0.col) ? cellOf(enemy0.row, enemy0.col) : -1,
        inside(enemy1.row, enemy1.col) ? cellOf(enemy1.row, enemy1.col) : -1,
    };
    const bool slowHand = likelySlowHand(state.speed, input);
    const bool robust = slowHand;
    if (slowHand) {
        for (const Position enemy : input.visible_enemies) {
            if (inside(enemy.row, enemy.col)) {
                markPreemptedNeighbors(npcCounts, enemy.row, enemy.col);
#if !defined(GOLD_ABLATE_ENEMY_PREDICTOR)
                const int predicted = predictEnemyEndpoint(input, enemy);
                if (predicted >= 0) {
                    npcCounts[predicted] |= kPredictedEnemyFlag;
                }
#endif
                if (!enemyCanVacate(input, enemy)) {
                    npcCounts[cellOf(enemy.row, enemy.col)] |=
                        kStationaryEnemyFlag;
                }
            }
        }
        // 快方行动时我方仍占据这两个格子，敌方角色无法进入并预先采金；
        // NPC 可以重叠，因此在下方处理 NPC 时仍允许重新标记。
        npcCounts[cellOf(input.my_units[0].row, input.my_units[0].col)] &=
            kNpcCountMask;
        npcCounts[cellOf(input.my_units[1].row, input.my_units[1].col)] &=
            kNpcCountMask;
    }
    int npcLimit = input.num_visible_npcs;
    if (npcLimit < 0) npcLimit = 0;
    if (npcLimit > MAX_NPCS) npcLimit = MAX_NPCS;
    const bool needNpcReach =
#if defined(GOLD_ABLATE_NPC_REACH)
        false;
#else
        slowHand && (input.my_units_gold[0] >= 600 ||
                     input.my_units_gold[1] >= 600);
#endif
    for (int index = 0; index < npcLimit; ++index) {
        const Position position = input.visible_npcs[index].pos;
        if (inside(position.row, position.col)) {
            ++npcCounts[cellOf(position.row, position.col)];
            if (slowHand) {
                markPreemptedNeighbors(npcCounts, position.row, position.col);
                if (needNpcReach) {
                    markNpcThreeStepReach(
                        reachRows, position.row, position.col);
                }
            }
        }
    }
    unsigned char legalLow[kCells];
    unsigned char legalHigh[kCells];
    buildLegalTables(ground, npcCounts, robust, enemyCells, legalLow,
                     legalHigh);
    const Target macro0 = macroWaypoint(input, 0);
    const Target macro1 = macroWaypoint(input, 1);
    LocalGoldScan scan0;
    LocalGoldScan scan1;
    scanLocalGold(input, 0, npcCounts, ground, legalLow, legalHigh, scan0);
    scanLocalGold(input, 1, npcCounts, ground, legalLow, legalHigh, scan1);
    int allocation = chooseAllocation(scan0, scan1);
    const bool emergency0 = npcPackEmergency(input, 0, robust, npcCounts);
    const bool emergency1 = npcPackEmergency(input, 1, robust, npcCounts);
    if (emergency0 != emergency1) {
        allocation = emergency0 ? 1 : 5;
    }
    Target gold0 = selectGoldWaypoint<false>(
        input.my_units[0], scan0, npcCounts, ground, allocation);
    Target initialGold1 = selectGoldWaypoint<false>(
        input.my_units[1], scan1, npcCounts, ground, 6 - allocation);
    const int start1 = cellOf(input.my_units[1].row, input.my_units[1].col);
    int actions1 = 6 - allocation;
    Target gold1 = initialGold1;
    Target target0 = gold0.gold > 0 ? gold0 : macro0;
    Route route0 = makeRoute(
        input, 0, target0, macro0, start1, -1, reachRows, npcCounts, ground,
        legalLow, legalHigh, allocation, robust, emergency0, state.enemies);

    // 固定角色0先执行时，若它最终停在双方共同目标上，尝试多给一步。
    // 只有实际重放确认新路线腾出了角色1目标才接受，避免用距离奇偶猜测。
    int watchedCell =
        gold1.gold > 0 ? cellOf(gold1.row, gold1.col) : -1;
    if (!emergency0 && !emergency1 && allocation < 5 && watchedCell >= 0 &&
        route0.endCell == watchedCell && gold0.row == gold1.row &&
        gold0.col == gold1.col) {
        const int originalAllocation = allocation;
        const Target originalGold0 = gold0;
        const Target originalGold1 = gold1;
        ++allocation;
        actions1 = 6 - allocation;
        std::memcpy(ground, input.grid, sizeof(ground));
        gold0 = selectGoldWaypoint<false>(
            input.my_units[0], scan0, npcCounts, ground, allocation);
        gold1 = selectGoldWaypoint<false>(
            input.my_units[1], scan1, npcCounts, ground, actions1);
        target0 = gold0.gold > 0 ? gold0 : macro0;
        route0 = makeRoute(
            input, 0, target0, macro0, start1, -1, reachRows, npcCounts,
            ground, legalLow, legalHigh, allocation, robust, emergency0,
            state.enemies);
        watchedCell =
            gold1.gold > 0 ? cellOf(gold1.row, gold1.col) : -1;
        if (watchedCell >= 0 && route0.endCell == watchedCell) {
            allocation = originalAllocation;
            actions1 = 6 - allocation;
            gold0 = originalGold0;
            gold1 = originalGold1;
            target0 = gold0.gold > 0 ? gold0 : macro0;
            std::memcpy(ground, input.grid, sizeof(ground));
            route0 = makeRoute(
                input, 0, target0, macro0, start1, -1, reachRows, npcCounts,
                ground, legalLow, legalHigh, allocation, robust, emergency0,
                state.enemies);
            watchedCell =
                gold1.gold > 0 ? cellOf(gold1.row, gold1.col) : -1;
        }
    }
    const int watchedGold = gold1.gold;
    if (watchedCell >= 0 &&
        (watchedCell == route0.endCell ||
         effectiveGold(ground[watchedCell], npcCounts[watchedCell]) !=
             watchedGold)) {
        gold1 = selectGoldWaypoint<true>(
            input.my_units[1], scan1, npcCounts, ground, actions1,
            route0.endCell);
    }
    const Target target1 = gold1.gold > 0 ? gold1 : macro1;
    const Route route1 = makeRoute(
        input, 1, target1, macro1, route0.endCell, -1, reachRows, npcCounts,
        ground, legalLow, legalHigh, actions1, robust, emergency1,
        state.enemies);

    GameOutput output;
    output.k = allocation;
    output.order = 0;
    output.vp = 0;
    for (int step = 0; step < allocation; ++step) {
        output.actions[step] = route0.actions[step];
    }
    for (int step = 0; step < actions1; ++step) {
        output.actions[step + allocation] = route1.actions[step];
    }
    rememberExpectedGain(state.speed, input,
                         static_cast<long long>(route0.expectedGain) +
                             route1.expectedGain);
    return output;
}

}  // namespace

extern "C" __attribute__((visibility("default")))
GameOutput moveDecision(const GameInput* input) {
    return input == nullptr ? safeOutput() : decide(*input);
}
