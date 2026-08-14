#include "game_api.h"

#include <climits>
#include <cstdint>

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(Position) == 8, "Position ABI mismatch");
static_assert(sizeof(NpcInfo) == 12, "NpcInfo ABI mismatch");
static_assert(sizeof(RegionStat) == 28, "RegionStat ABI mismatch");
static_assert(sizeof(Snapshot) == 148, "Snapshot ABI mismatch");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

namespace {

constexpr int kSide = GRID_SIZE;
constexpr int kStay = 4;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

struct Point {
    int row;
    int col;
};

struct Target {
    int cell;
    int value;
};

struct Route {
    std::uint8_t actions[S];
    Point end;
};

struct Blockers {
    int enemy0;
    int enemy1;
    int crowded0;
    int crowded1;
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

inline int clampedGold(int value) noexcept {
    return value > 0 ? (value < 1000000 ? value : 1000000) : 0;
}

GameOutput fallbackOutput() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

Blockers makeBlockers(const GameInput& input) noexcept {
    Blockers result{-1, -1, -1, -1};
    if (validPosition(input.visible_enemies[0])) {
        result.enemy0 = cellOf(input.visible_enemies[0].row,
                               input.visible_enemies[0].col);
    }
    if (validPosition(input.visible_enemies[1])) {
        result.enemy1 = cellOf(input.visible_enemies[1].row,
                               input.visible_enemies[1].col);
    }

    int npcCells[MAX_NPCS];
    int npcCount = input.num_visible_npcs;
    if (npcCount < 0) npcCount = 0;
    if (npcCount > MAX_NPCS) npcCount = MAX_NPCS;
    int validCount = 0;
    for (int index = 0; index < npcCount; ++index) {
        const Position position = input.visible_npcs[index].pos;
        if (validPosition(position)) {
            npcCells[validCount++] = cellOf(position.row, position.col);
        }
    }
    for (int index = 0; index < validCount; ++index) {
        const int cell = npcCells[index];
        if (cell == result.crowded0) continue;
        int count = 1;
        for (int other = index + 1; other < validCount; ++other) {
            count += npcCells[other] == cell;
        }
        if (count < 3) continue;
        if (result.crowded0 < 0) {
            result.crowded0 = cell;
        } else {
            result.crowded1 = cell;
            break;
        }
    }
    return result;
}

inline bool openCell(const GameInput& input, const Blockers& blockers,
                     int row, int col, int ownBlock) noexcept {
    if (!inside(row, col)) return false;
    const int cell = cellOf(row, col);
    if (cell == ownBlock || cell == blockers.enemy0 ||
        cell == blockers.enemy1 || cell == blockers.crowded0 ||
        cell == blockers.crowded1) {
        return false;
    }
    const int tile = input.grid[row][col];
    return tile != -5 && tile != -1 && tile != -3;
}

Target scanGuard(const GameInput& input, const Blockers& blockers,
                 Point start, int ownBlock) noexcept {
    Target best{-1, 0};
    int bestScore = 0;
    for (int dr = -1; dr <= 1; ++dr) {
        const int row = start.row + dr;
        if (static_cast<unsigned>(row) >= static_cast<unsigned>(kSide)) {
            continue;
        }
        for (int dc = -1; dc <= 1; ++dc) {
            const int col = start.col + dc;
            if (static_cast<unsigned>(col) >= static_cast<unsigned>(kSide)) {
                continue;
            }
            if ((dr != 0 || dc != 0) &&
                !openCell(input, blockers, row, col, ownBlock)) {
                continue;
            }
            const int value = clampedGold(input.grid[row][col]);
            const int score = value * 4 - absInt(dr) - absInt(dc);
            if (score > bestScore) {
                best = Target{cellOf(row, col), value};
                bestScore = score;
            }
        }
    }
    return best;
}

Target scanMain(const GameInput& input, const Blockers& blockers,
                Point start, int ownBlock) noexcept {
    Target best{-1, 0};
    int bestScore = 0;
    for (int dr = -2; dr <= 2; ++dr) {
        const int row = start.row + dr;
        if (static_cast<unsigned>(row) >= static_cast<unsigned>(kSide)) {
            continue;
        }
        for (int dc = -2; dc <= 2; ++dc) {
            const int col = start.col + dc;
            if (static_cast<unsigned>(col) >= static_cast<unsigned>(kSide)) {
                continue;
            }
            const int cell = cellOf(row, col);
            if ((dr != 0 || dc != 0) &&
                !openCell(input, blockers, row, col, ownBlock)) {
                continue;
            }
            const int value = clampedGold(input.grid[row][col]);
            const int score = value * 4 - absInt(dr) - absInt(dc);
            if (score > bestScore) {
                best = Target{cell, value};
                bestScore = score;
            }
        }
    }
    return best;
}

inline Point pointOf(int cell) noexcept {
    const int row = cell / kSide;
    return Point{row, cell - row * kSide};
}

int chooseAction(const GameInput& input, const Blockers& blockers,
                 Point current, Point destination, int ownBlock,
                 int previous) noexcept {
    int bestAction = kStay;
    int bestScore = INT_MIN;
    for (int action = 0; action < 4; ++action) {
        const int row = current.row + kDr[action];
        const int col = current.col + kDc[action];
        if (!openCell(input, blockers, row, col, ownBlock)) continue;
        const int distance = absInt(destination.row - row) +
                             absInt(destination.col - col);
        const int value = clampedGold(input.grid[row][col]);
        int score = value * 64 - distance * 16;
        if (previous >= 0 && (previous ^ 1) == action) score -= 1;
        if (score > bestScore) {
            bestScore = score;
            bestAction = action;
        }
    }
    return bestAction;
}

Route makeRoute(const GameInput& input, const Blockers& blockers,
                Point start, int ownBlock, Target target, Point macro,
                int actionCount) noexcept {
    Route route{{kStay, kStay, kStay, kStay, kStay, kStay}, start};
    int previous = -1;
    for (int step = 0; step < actionCount; ++step) {
        Point destination = macro;
        if (target.cell >= 0) {
            const Point gold = pointOf(target.cell);
            if (route.end.row != gold.row || route.end.col != gold.col) {
                destination = gold;
            }
        }
        const int action = chooseAction(input, blockers, route.end,
                                        destination, ownBlock, previous);
        route.actions[step] = static_cast<std::uint8_t>(action);
        if (action != kStay) {
            route.end.row += kDr[action];
            route.end.col += kDc[action];
            previous = action;
        }
    }
    return route;
}

Point mainMacro(int round) noexcept {
    static constexpr Point ring[8] = {
        {7, 7}, {7, 8}, {7, 9}, {8, 9},
        {9, 9}, {9, 8}, {9, 7}, {8, 7},
    };
    return ring[(round >> 1) & 7];
}

GameOutput decide(const GameInput& input) noexcept {
    const Blockers blockers = makeBlockers(input);
    const Point starts[2] = {
        {input.my_units[0].row, input.my_units[0].col},
        {input.my_units[1].row, input.my_units[1].col},
    };
    const int startCells[2] = {
        cellOf(starts[0].row, starts[0].col),
        cellOf(starts[1].row, starts[1].col),
    };

    Target guard = scanGuard(input, blockers, starts[0], startCells[1]);
    Target main = scanMain(input, blockers, starts[1], startCells[0]);
    const int guardDistance = absInt(starts[0].row - 8) +
                              absInt(starts[0].col - 8);
    int allocation = 0;
    if (guard.value > 0) {
        const Point gold = pointOf(guard.cell);
        const int distance = absInt(gold.row - starts[0].row) +
                             absInt(gold.col - starts[0].col);
        allocation = distance == 0 ? 2 : (distance == 1 ? 3 : 2);
    } else if (guardDistance > 0) {
        allocation = 2;
    } else if (main.value <= 0 && input.round % 3 == 0) {
        allocation = 2;
    }

    const int mainBudget = S - allocation;
    const Point mainGoal = mainMacro(input.round);
    int mainSteps = mainBudget;
    if (main.value <= 0) {
        if (mainSteps > 2) mainSteps = 2;
    }

    const int order = allocation > 0 && guard.value > main.value ? 0 : 1;
    Route routes[2]{};
    if (order == 0) {
        const Target guardTarget = guard.value > 0 ? guard : Target{-1, 0};
        routes[0] = makeRoute(input, blockers, starts[0], startCells[1],
                              guardTarget, Point{8, 8}, allocation);
        const int guardEnd = cellOf(routes[0].end.row, routes[0].end.col);
        if (main.cell == guardEnd) {
            main = scanMain(input, blockers, starts[1], guardEnd);
        }
        routes[1] = makeRoute(
            input, blockers, starts[1], guardEnd, main, mainGoal, mainSteps);
    } else {
        routes[1] = makeRoute(input, blockers, starts[1], startCells[0],
                              main, mainGoal, mainSteps);
        const int mainEnd = cellOf(routes[1].end.row, routes[1].end.col);
        if (guard.cell == mainEnd) {
            guard = scanGuard(input, blockers, starts[0], mainEnd);
        }
        const Target guardTarget = guard.value > 0 ? guard : Target{-1, 0};
        routes[0] = makeRoute(input, blockers, starts[0], mainEnd,
                              guardTarget, Point{8, 8}, allocation);
    }

    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay},
                      allocation, order, 0};
    for (int step = 0; step < allocation; ++step) {
        output.actions[step] = routes[0].actions[step];
    }
    for (int step = 0; step < mainBudget; ++step) {
        output.actions[allocation + step] = routes[1].actions[step];
    }
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
