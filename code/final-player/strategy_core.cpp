#include "strategy_core.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

extern "C" GameOutput legacyMoveDecision(const GameInput* input);

namespace final_player {
namespace {

constexpr int kCells = GRID_SIZE * GRID_SIZE;
constexpr int kStay = 4;
constexpr int kUnknown = -1;
constexpr int kOpen = 0;
constexpr int kWall = 1;
constexpr int kFar = 10000;
constexpr int kRouteCount = 125;  // 5^3
constexpr int kRoutesKept = 28;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

constexpr const char* kMazeRows[GRID_SIZE] = {
    "..#...#...#...#..", ".##.#.##.##.#.##.",
    "....#.......#....", ".#######.#######.",
    ".......#.#.......", "######.#.#.######",
    ".....#.....#.....", ".###.###.###.###.",
    "...#.........#...", ".#.#.###.###.#.#.",
    ".#.#.#.....#.#.#.", ".#.#.#.#.#.#.#.#.",
    ".#...#.#.#.#...#.", ".###.#.###.#.###.",
    ".................", ".###.###.###.###.",
    "..#...#...#...#.."};

constexpr Position kHotspots[6] = {
    {0, 8}, {2, 3}, {2, 13}, {16, 3}, {16, 8}, {16, 13}};

constexpr Position kPatrol[13] = {
    {6, 4},  {6, 6},  {6, 8},  {6, 10}, {6, 12}, {8, 12},
    {10, 12}, {10, 10}, {10, 8}, {10, 6}, {10, 4}, {8, 4},
    {8, 8}};

inline bool inside(int row, int col) noexcept {
    return static_cast<unsigned>(row) < static_cast<unsigned>(GRID_SIZE) &&
           static_cast<unsigned>(col) < static_cast<unsigned>(GRID_SIZE);
}

inline int cellOf(int row, int col) noexcept {
    return row * GRID_SIZE + col;
}

inline int cellOf(Position position) noexcept {
    return inside(position.row, position.col)
               ? cellOf(position.row, position.col)
               : -1;
}

inline Position positionOf(int cell) noexcept {
    return Position{cell / GRID_SIZE, cell % GRID_SIZE};
}

inline int absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

inline int clampNonnegative(int value, int upper = 1000000) noexcept {
    if (value <= 0) return 0;
    return value > upper ? upper : value;
}

inline int narrowScore(std::int64_t value) noexcept {
    if (value > INT_MAX) return INT_MAX;
    if (value < INT_MIN) return INT_MIN;
    return static_cast<int>(value);
}

inline bool same(Position first, Position second) noexcept {
    return first.row == second.row && first.col == second.col;
}

inline int regionOf(Position position) noexcept {
    if (position.row >= 4 && position.row <= 12 && position.col >= 4 &&
        position.col <= 12) {
        return 1;
    }
    if (position.row <= 3 && position.col <= 12) return 2;
    if (position.row >= 4 && position.col <= 3) return 3;
    if (position.row >= 13 && position.col >= 4) return 4;
    return 5;
}

GameOutput fallback() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

bool validInput(const GameInput* input) noexcept {
    return input != nullptr && input->round >= 0 &&
           input->round <= 1000000 &&
           inside(input->my_units[0].row, input->my_units[0].col) &&
           inside(input->my_units[1].row, input->my_units[1].col) &&
           !same(input->my_units[0], input->my_units[1]);
}

struct Memory {
    bool initialized;
    int lastRound;
    bool deep;
    bool exactMaze;
    int outerUnit;
    int centerUnit;
    int homeHotspot;
    int outerGoal;
    bool predictionValid;
    Position predicted[2];
    int blockEvidence;
    int terrain[kCells];
    int lastSeen[kCells];
    int lastValue[kCells];
    int lastVisit[kCells];
    int goldEvidence[kCells];
    RegionStat regions[REGION_COUNT];
    bool hasSnapshot;
};

Memory gMemory{};

int visibleWallCount(const GameInput& input) noexcept {
    int walls = 0;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            walls += input.grid[row][col] == -1;
        }
    }
    return walls;
}

bool matchesMaze(const GameInput& input) noexcept {
    int seen = 0;
    int walls = 0;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value == -5) continue;
            ++seen;
            const bool observedWall = value == -1;
            const bool expectedWall = kMazeRows[row][col] == '#';
            if (observedWall != expectedWall) return false;
            walls += observedWall;
        }
    }
    return seen >= 12 && walls >= 4;
}

void resetMemory(const GameInput& input, Profile profile) noexcept {
    gMemory.initialized = true;
    gMemory.lastRound = input.round;
    gMemory.exactMaze = matchesMaze(input);
    const int wallCount = visibleWallCount(input);
    gMemory.deep = profile == Profile::kAlwaysDeep || gMemory.exactMaze ||
                   wallCount >= 5;
    gMemory.outerUnit = input.my_units[0].row <= input.my_units[1].row ? 0 : 1;
    gMemory.centerUnit = gMemory.outerUnit ^ 1;
    const Position outerStart = input.my_units[gMemory.outerUnit];
    const Position sideGoal = outerStart.col < GRID_SIZE / 2
                                  ? Position{2, 3}
                                  : Position{2, 13};
    gMemory.homeHotspot = cellOf(sideGoal);
    gMemory.outerGoal = gMemory.homeHotspot;
    gMemory.predictionValid = false;
    gMemory.predicted[0] = input.my_units[0];
    gMemory.predicted[1] = input.my_units[1];
    gMemory.blockEvidence = 0;
    gMemory.hasSnapshot = false;
    for (int cell = 0; cell < kCells; ++cell) {
        gMemory.terrain[cell] = kUnknown;
        gMemory.lastSeen[cell] = -1000000;
        gMemory.lastValue[cell] = -5;
        gMemory.lastVisit[cell] = -1000000;
        gMemory.goldEvidence[cell] = 0;
    }
    if (gMemory.exactMaze) {
        for (int row = 0; row < GRID_SIZE; ++row) {
            for (int col = 0; col < GRID_SIZE; ++col) {
                gMemory.terrain[cellOf(row, col)] =
                    kMazeRows[row][col] == '#' ? kWall : kOpen;
            }
        }
    }
    for (RegionStat& region : gMemory.regions) region = RegionStat{};
}

void updateMemory(const GameInput& input) noexcept {
    if (gMemory.predictionValid) {
        const bool mismatch = !same(gMemory.predicted[0], input.my_units[0]) ||
                              !same(gMemory.predicted[1], input.my_units[1]);
        if (mismatch) {
            gMemory.blockEvidence = std::min(10, gMemory.blockEvidence + 2);
        } else {
            gMemory.blockEvidence = std::max(0, gMemory.blockEvidence - 1);
        }
        gMemory.predictionValid = false;
    }
    if (gMemory.exactMaze) {
        bool mismatch = false;
        for (int row = 0; row < GRID_SIZE && !mismatch; ++row) {
            for (int col = 0; col < GRID_SIZE; ++col) {
                const int value = input.grid[row][col];
                if (value == -5) continue;
                if ((value == -1) != (kMazeRows[row][col] == '#')) {
                    mismatch = true;
                    break;
                }
            }
        }
        if (mismatch) {
            gMemory.exactMaze = false;
            gMemory.predictionValid = false;
            for (int cell = 0; cell < kCells; ++cell) {
                if (gMemory.lastSeen[cell] < 0) {
                    gMemory.terrain[cell] = kUnknown;
                }
            }
        }
    }
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value == -5) continue;
            const int cell = cellOf(row, col);
            const int oldValue = gMemory.lastValue[cell];
            const int oldRound = gMemory.lastSeen[cell];
            if (value == -1) {
                gMemory.terrain[cell] = kWall;
            } else if (value == -3 || value >= 0) {
                gMemory.terrain[cell] = kOpen;
            } else {
                gMemory.terrain[cell] = kUnknown;
            }
            if (value > 0 &&
                (oldRound + 1 != input.round || oldValue <= 0 ||
                 value > oldValue)) {
                const int evidence = value > oldValue && oldValue > 0
                                         ? value - oldValue
                                         : value;
                const int capped = evidence > 1000 ? 1000 : evidence;
                gMemory.goldEvidence[cell] =
                    std::min(100000, gMemory.goldEvidence[cell] + capped);
            }
            gMemory.lastSeen[cell] = input.round;
            gMemory.lastValue[cell] = value;
        }
    }
    for (Position unit : input.my_units) {
        gMemory.lastVisit[cellOf(unit)] = input.round;
    }
    if (input.snapshot_valid == 1) {
        bool valid = true;
        bool used[REGION_COUNT] = {false, false, false, false, false};
        for (const RegionStat& region : input.snapshot.regions) {
            if (region.id < 1 || region.id > REGION_COUNT ||
                used[region.id - 1]) {
                valid = false;
                break;
            }
            used[region.id - 1] = true;
        }
        if (valid) {
            for (const RegionStat& region : input.snapshot.regions) {
                gMemory.regions[region.id - 1] = region;
            }
            gMemory.hasSnapshot = true;
        }
    }
    gMemory.lastRound = input.round;
}

bool currentBomb(const GameInput& input, int cell) noexcept {
    const Position position = positionOf(cell);
    if (input.grid[position.row][position.col] == -3) return true;
    return gMemory.lastValue[cell] == -3 &&
           gMemory.lastSeen[cell] / 20 == input.round / 20;
}

bool visibleEnemy(const GameInput& input, int cell) noexcept {
    return cell == cellOf(input.visible_enemies[0]) ||
           cell == cellOf(input.visible_enemies[1]);
}

bool traversable(const GameInput& input, int cell) noexcept {
    return cell >= 0 && cell < kCells && gMemory.terrain[cell] == kOpen &&
           !currentBomb(input, cell) && !visibleEnemy(input, cell);
}

void buildDistances(const GameInput& input, int goal,
                    int distances[kCells]) noexcept {
    for (int cell = 0; cell < kCells; ++cell) distances[cell] = kFar;
    if (!traversable(input, goal)) return;
    int queue[kCells];
    int begin = 0;
    int end = 0;
    queue[end++] = goal;
    distances[goal] = 0;
    while (begin < end) {
        const int cell = queue[begin++];
        const Position position = positionOf(cell);
        for (int action = 0; action < 4; ++action) {
            const int row = position.row + kDr[action];
            const int col = position.col + kDc[action];
            if (!inside(row, col)) continue;
            const int next = cellOf(row, col);
            if (!traversable(input, next) || distances[next] != kFar) {
                continue;
            }
            distances[next] = distances[cell] + 1;
            queue[end++] = next;
        }
    }
}

int bestVisibleGold(const GameInput& input, const int distances[kCells],
                    int requiredRegion, int maxDistance) noexcept {
    int bestCell = -1;
    int bestScore = INT_MIN;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value <= 0) continue;
            const Position position{row, col};
            if (requiredRegion > 0 && regionOf(position) != requiredRegion) {
                continue;
            }
            const int cell = cellOf(position);
            const int distance = distances[cell];
            if (distance > maxDistance) continue;
            const int cappedValue = value > 1000000 ? 1000000 : value;
            const int score = cappedValue * 32 - distance * 15;
            if (score > bestScore) {
                bestScore = score;
                bestCell = cell;
            }
        }
    }
    return bestCell;
}

int nearestOpenToCenter(const GameInput& input,
                        const int distances[kCells], int start) noexcept {
    int best = start;
    int bestScore = INT_MAX;
    for (int cell = 0; cell < kCells; ++cell) {
        if (!traversable(input, cell)) continue;
        const Position position = positionOf(cell);
        const int centerDistance = absInt(position.row - 8) +
                                   absInt(position.col - 8);
        const int routeDistance = distances[cell];
        if (routeDistance >= kFar) continue;
        const int score = centerDistance * 32 + routeDistance;
        if (score < bestScore) {
            bestScore = score;
            best = cell;
        }
    }
    return best;
}

int patrolGoal(const GameInput& input, int unit,
               const int distances[kCells], int start,
               int avoidGoal) noexcept {
    int best = -1;
    int bestScore = INT_MIN;
    for (int index = 0; index < static_cast<int>(std::size(kPatrol));
         ++index) {
        const int cell = cellOf(kPatrol[index]);
        if (cell == avoidGoal || !traversable(input, cell)) continue;
        const int distance = distances[cell];
        if (distance >= kFar) continue;
        int stale = input.round - gMemory.lastVisit[cell];
        if (stale > 1000) stale = 1000;
        const int phase = (index - input.round / 7 - unit * 5) & 15;
        const int score = stale * 8 - distance * 24 - phase;
        if (score > bestScore) {
            bestScore = score;
            best = cell;
        }
    }
    return best >= 0 ? best
                     : nearestOpenToCenter(input, distances, start);
}

struct Goals {
    int cell[2];
};

Goals chooseGoals(const GameInput& input, bool useHotspots) noexcept {
    Goals goals{{-1, -1}};
    const int starts[2] = {cellOf(input.my_units[0]),
                           cellOf(input.my_units[1])};
    int fromStart[2][kCells];
    buildDistances(input, starts[0], fromStart[0]);
    buildDistances(input, starts[1], fromStart[1]);

    if (gMemory.exactMaze && useHotspots) {
        const int outer = gMemory.outerUnit;
        const int center = gMemory.centerUnit;
        int outerGoal = gMemory.outerGoal;
        int bestHotspotScore = INT_MIN;
        for (Position hotspot : kHotspots) {
            const int cell = cellOf(hotspot);
            const int distance = fromStart[outer][cell];
            if (distance >= kFar) continue;
            const int region = regionOf(hotspot) - 1;
            int score = -distance * 18;
            score += cell == gMemory.outerGoal ? 50 : 0;
            score += cell == gMemory.homeHotspot ? 100 : 0;
            if (gMemory.hasSnapshot) {
                score += clampNonnegative(
                             gMemory.regions[region].gold_remaining) *
                         3;
                score -= clampNonnegative(gMemory.regions[region].occupants,
                                          1000) *
                         12;
            }
            score += std::min(500, gMemory.goldEvidence[cell] * 2);
            const Position position = positionOf(cell);
            const int visibleValue = input.grid[position.row][position.col];
            if (visibleValue > 0) {
                score += std::min(1000000, visibleValue) * 64;
            } else if (visibleValue != -5) {
                score -= 80;
            }
            if (score > bestHotspotScore) {
                bestHotspotScore = score;
                outerGoal = cell;
            }
        }
        gMemory.outerGoal = outerGoal;
        const int outerRegion = regionOf(positionOf(outerGoal));
        const int localGold = bestVisibleGold(
            input, fromStart[outer], outerRegion, 7);
        if (localGold >= 0) outerGoal = localGold;
        goals.cell[outer] = outerGoal;

        int centerGoal =
            bestVisibleGold(input, fromStart[center], 1, 12);
        if (centerGoal < 0) {
            centerGoal = patrolGoal(input, center, fromStart[center],
                                    starts[center], outerGoal);
        }
        goals.cell[center] = centerGoal;
        return goals;
    }

    for (int unit = 0; unit < 2; ++unit) {
        int goal = bestVisibleGold(input, fromStart[unit], 0, 12);
        if (goal < 0) {
            if (gMemory.exactMaze) {
                goal = patrolGoal(input, unit, fromStart[unit], starts[unit],
                                  unit == 1 ? goals.cell[0] : -1);
            } else {
                goal = nearestOpenToCenter(input, fromStart[unit],
                                           starts[unit]);
            }
        }
        goals.cell[unit] = goal;
    }
    if (goals.cell[0] == goals.cell[1] && gMemory.exactMaze) {
        goals.cell[1] = patrolGoal(input, 1, fromStart[1], starts[1],
                                   goals.cell[0]);
    }
    return goals;
}

void npcCounts(const GameInput& input, int counts[kCells]) noexcept {
    for (int cell = 0; cell < kCells; ++cell) counts[cell] = 0;
    int count = input.num_visible_npcs;
    if (count < 0) count = 0;
    if (count > MAX_NPCS) count = MAX_NPCS;
    for (int index = 0; index < count; ++index) {
        const NpcInfo& npc = input.visible_npcs[index];
        const int cell = npc.id != 0 ? cellOf(npc.pos) : -1;
        if (cell >= 0) ++counts[cell];
    }
}

struct Route {
    std::uint8_t actions[3];
    int endCell;
    int score;
};

int coinValue(const GameInput& input, int cell) noexcept {
    const Position position = positionOf(cell);
    const int value = input.grid[position.row][position.col];
    if (value <= 0) return 0;
    return value > 1000000 ? 1000000 : value;
}

int takeCoin(int cell, const GameInput& input, int cells[3], int values[3],
             int& used) noexcept {
    int slot = -1;
    for (int index = 0; index < used; ++index) {
        if (cells[index] == cell) slot = index;
    }
    if (slot < 0) {
        if (used >= 3) return 0;
        slot = used++;
        cells[slot] = cell;
        values[slot] = coinValue(input, cell);
    }
    const int remaining = values[slot];
    if (remaining <= 0) return 0;
    const int pickup = (remaining * 65 + 99) / 100;
    values[slot] -= pickup;
    return pickup;
}

Route makeRoute(const GameInput& input, int unit, int code,
                const int distances[kCells],
                const int crowded[kCells]) noexcept {
    Route route{{kStay, kStay, kStay}, cellOf(input.my_units[unit]),
                INT_MIN};
    int position = route.endCell;
    std::int64_t held = clampNonnegative(input.my_units_gold[unit], INT_MAX);
    std::int64_t pickupTotal = 0;
    std::int64_t lossTotal = 0;
    int moves = 0;
    int exploration = 0;
    int coinCells[3] = {-1, -1, -1};
    int coinValues[3] = {0, 0, 0};
    int usedCoins = 0;
    int encoded = code;
    for (int step = 0; step < 3; ++step) {
        const int action = encoded % 5;
        encoded /= 5;
        route.actions[step] = static_cast<std::uint8_t>(action);
        if (action == kStay) continue;
        const Position current = positionOf(position);
        const int row = current.row + kDr[action];
        const int col = current.col + kDc[action];
        if (!inside(row, col)) return route;
        const int next = cellOf(row, col);
        if (!traversable(input, next)) return route;
        position = next;
        ++moves;
        if (gMemory.lastSeen[next] < input.round) exploration += 2;
        exploration += gMemory.lastVisit[next] + 10 < input.round;
        const int pickup = takeCoin(next, input, coinCells, coinValues,
                                    usedCoins);
        pickupTotal += pickup;
        held += pickup;
        if (crowded[next] >= 3) {
            const std::int64_t loss = (held + 19) / 20;
            held -= loss;
            lossTotal += loss;
        }
    }
    route.endCell = position;
    const int distance = distances[position];
    const int distancePenalty = distance >= kFar ? 200000 : distance * 56;
    route.score = narrowScore(pickupTotal * 256 - lossTotal * 336 -
                              distancePenalty + moves * 5 + exploration * 7);
    return route;
}

bool prefixSafe(const GameInput& input, int unit,
                const Route& route) noexcept {
    int candidates[3] = {-1, -1, -1};
    int candidateCount = 0;
    int normal = cellOf(input.my_units[unit]);
    for (int step = 0; step < 3; ++step) {
        const int action = route.actions[step];
        if (action == kStay) continue;
        const Position position = positionOf(normal);
        const int row = position.row + kDr[action];
        const int col = position.col + kDc[action];
        if (!inside(row, col)) return false;
        const int next = cellOf(row, col);
        if (!traversable(input, next)) return false;
        bool known = false;
        for (int index = 0; index < candidateCount; ++index) {
            known |= candidates[index] == next;
        }
        if (!known) candidates[candidateCount++] = next;
        normal = next;
    }
    for (int candidate = 0; candidate < candidateCount; ++candidate) {
        int position = cellOf(input.my_units[unit]);
        for (int step = 0; step < 3; ++step) {
            const int action = route.actions[step];
            if (action == kStay) continue;
            const Position current = positionOf(position);
            const int row = current.row + kDr[action];
            const int col = current.col + kDc[action];
            if (!inside(row, col)) return false;
            const int next = cellOf(row, col);
            if (next == candidates[candidate]) continue;
            if (!traversable(input, next)) return false;
            position = next;
        }
    }
    return true;
}

int generateRoutes(const GameInput& input, int unit,
                   const int distances[kCells], const int crowded[kCells],
                   int keepLimit, Route kept[kRoutesKept]) noexcept {
    Route all[kRouteCount];
    int valid = 0;
    for (int code = 0; code < kRouteCount; ++code) {
        const Route route = makeRoute(input, unit, code, distances, crowded);
        if (route.score != INT_MIN &&
            (gMemory.blockEvidence < 2 || prefixSafe(input, unit, route))) {
            all[valid++] = route;
        }
    }
    std::sort(all, all + valid,
              [](const Route& first, const Route& second) noexcept {
                  if (first.score != second.score) {
                      return first.score > second.score;
                  }
                  const int firstCode = first.actions[0] +
                                        first.actions[1] * 5 +
                                        first.actions[2] * 25;
                  const int secondCode = second.actions[0] +
                                         second.actions[1] * 5 +
                                         second.actions[2] * 25;
                  return firstCode < secondCode;
              });
    const int count = std::min(valid, std::min(keepLimit, kRoutesKept));
    for (int index = 0; index < count; ++index) kept[index] = all[index];
    if (count > 0) {
        int replacement = count - 1;
        for (int code = kRouteCount - 5; code < kRouteCount; ++code) {
            const Route anchor =
                makeRoute(input, unit, code, distances, crowded);
            if (anchor.score == INT_MIN ||
                (gMemory.blockEvidence >= 2 &&
                 !prefixSafe(input, unit, anchor))) {
                continue;
            }
            bool present = false;
            for (int index = 0; index < count; ++index) {
                present |= kept[index].actions[0] == anchor.actions[0] &&
                           kept[index].actions[1] == anchor.actions[1] &&
                           kept[index].actions[2] == anchor.actions[2];
            }
            if (!present && replacement >= 0) {
                kept[replacement--] = anchor;
            }
        }
    }
    return count;
}

struct PairResult {
    int score;
    int order;
    int firstRoute;
    int secondRoute;
};

int sharedPickup(int cell, const GameInput& input, int cells[S],
                 int values[S], int& used) noexcept {
    int slot = -1;
    for (int index = 0; index < used; ++index) {
        if (cells[index] == cell) slot = index;
    }
    if (slot < 0) {
        slot = used++;
        cells[slot] = cell;
        values[slot] = coinValue(input, cell);
    }
    const int remaining = values[slot];
    if (remaining <= 0) return 0;
    const int pickup = (remaining * 65 + 99) / 100;
    values[slot] -= pickup;
    return pickup;
}

int evaluatePair(const GameInput& input, const Route& route0,
                 const Route& route1, int order,
                 const int distances0[kCells],
                 const int distances1[kCells],
                 const int crowded[kCells]) noexcept {
    const Route* routes[2] = {&route0, &route1};
    int positions[2] = {cellOf(input.my_units[0]),
                        cellOf(input.my_units[1])};
    std::int64_t held[2] = {
        clampNonnegative(input.my_units_gold[0], INT_MAX),
        clampNonnegative(input.my_units_gold[1], INT_MAX)};
    int coinCells[S] = {-1, -1, -1, -1, -1, -1};
    int coinValues[S] = {0, 0, 0, 0, 0, 0};
    int usedCoins = 0;
    std::int64_t pickupTotal = 0;
    std::int64_t lossTotal = 0;
    int moves = 0;
    int blocked = 0;
    int exploration = 0;
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? order : order ^ 1;
        for (int step = 0; step < 3; ++step) {
            const int action = routes[unit]->actions[step];
            if (action == kStay) continue;
            const Position current = positionOf(positions[unit]);
            const int row = current.row + kDr[action];
            const int col = current.col + kDc[action];
            if (!inside(row, col)) {
                ++blocked;
                continue;
            }
            const int next = cellOf(row, col);
            if (!traversable(input, next) || next == positions[unit ^ 1]) {
                ++blocked;
                continue;
            }
            positions[unit] = next;
            ++moves;
            if (gMemory.lastSeen[next] < input.round) exploration += 2;
            exploration += gMemory.lastVisit[next] + 10 < input.round;
            const int pickup = sharedPickup(next, input, coinCells,
                                            coinValues, usedCoins);
            pickupTotal += pickup;
            held[unit] += pickup;
            if (crowded[next] >= 3) {
                const std::int64_t loss = (held[unit] + 19) / 20;
                held[unit] -= loss;
                lossTotal += loss;
            }
        }
    }
    const int distance0 = distances0[positions[0]];
    const int distance1 = distances1[positions[1]];
    if (blocked != 0 || distance0 >= kFar || distance1 >= kFar) {
        return INT_MIN;
    }
    return narrowScore(pickupTotal * 256 - lossTotal * 336 -
                       (distance0 + distance1) * 56 + moves * 5 +
                       exploration * 7);
}

GameOutput deepDecision(const GameInput& input, bool useHotspots,
                        int routeLimit) noexcept {
    const Goals goals = chooseGoals(input, useHotspots);
    int distances[2][kCells];
    for (int unit = 0; unit < 2; ++unit) {
        int goal = goals.cell[unit];
        if (!traversable(input, goal)) goal = cellOf(input.my_units[unit]);
        buildDistances(input, goal, distances[unit]);
    }
    int crowded[kCells];
    npcCounts(input, crowded);
    Route routes[2][kRoutesKept];
    const int counts[2] = {
        generateRoutes(input, 0, distances[0], crowded, routeLimit,
                       routes[0]),
        generateRoutes(input, 1, distances[1], crowded, routeLimit,
                       routes[1])};
    if (counts[0] <= 0 || counts[1] <= 0) return fallback();

    PairResult best{INT_MIN, 0, 0, 0};
    for (int order = 0; order < 2; ++order) {
        for (int first = 0; first < counts[0]; ++first) {
            for (int second = 0; second < counts[1]; ++second) {
                const int score = evaluatePair(
                    input, routes[0][first], routes[1][second], order,
                    distances[0], distances[1], crowded);
                if (score > best.score) {
                    best = PairResult{score, order, first, second};
                }
            }
        }
    }
    if (best.score == INT_MIN) return fallback();
    const Route& route0 = routes[0][best.firstRoute];
    const Route& route1 = routes[1][best.secondRoute];
    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay},
                      3, best.order, 0};
    for (int step = 0; step < 3; ++step) {
        output.actions[step] = route0.actions[step];
        output.actions[step + 3] = route1.actions[step];
    }
    return output;
}

void rememberPrediction(const GameInput& input,
                        const GameOutput& output) noexcept {
    Position positions[2] = {input.my_units[0], input.my_units[1]};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : output.order ^ 1;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) continue;
            const Position next{positions[unit].row + kDr[action],
                                positions[unit].col + kDc[action]};
            const int cell = cellOf(next);
            if (!inside(next.row, next.col) || !traversable(input, cell) ||
                same(next, positions[unit ^ 1])) {
                continue;
            }
            positions[unit] = next;
        }
    }
    gMemory.predicted[0] = positions[0];
    gMemory.predicted[1] = positions[1];
    gMemory.predictionValid = true;
}

}  // namespace

GameOutput decide(const GameInput* input, Profile profile) noexcept {
    if (!validInput(input)) return fallback();
    if (profile == Profile::kFastOnly) return legacyMoveDecision(input);

    const bool discontinuity = !gMemory.initialized || input->round == 0 ||
                               input->round != gMemory.lastRound + 1;
    if (discontinuity) resetMemory(*input, profile);
    if (profile == Profile::kAdaptive ||
        profile == Profile::kAdaptiveNoHotspots ||
        profile == Profile::kAdaptiveNoBlockInference) {
        if (!gMemory.deep) {
            gMemory.lastRound = input->round;
            return legacyMoveDecision(input);
        }
    } else {
        gMemory.deep = true;
    }
    const bool blockAware = profile != Profile::kAdaptiveNoBlockInference;
    if (!blockAware) {
        gMemory.predictionValid = false;
        gMemory.blockEvidence = 0;
    }
    updateMemory(*input);
    const bool useHotspots = profile != Profile::kAdaptiveNoHotspots;
    const int routeLimit = profile == Profile::kAlwaysDeep ? kRoutesKept : 8;
    const GameOutput output = deepDecision(*input, useHotspots, routeLimit);
    if (blockAware) rememberPrediction(*input, output);
    return output;
}

}  // namespace final_player
