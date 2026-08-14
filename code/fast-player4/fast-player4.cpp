#include "game_api.h"

#include <climits>
#include <immintrin.h>

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

namespace {

constexpr int kStay = 4;
constexpr int kCenter = 8;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

struct Blockers {
    int enemy0;
    int enemy1;
    int crowded0;
    int crowded1;
};

struct Target {
    int row;
    int col;
    int value;
    int utility;
    int distance;
};

struct Route {
    int actions[S];
    Position end;
};

inline bool inside(int row, int col) noexcept {
    return static_cast<unsigned>(row) < static_cast<unsigned>(GRID_SIZE) &&
           static_cast<unsigned>(col) < static_cast<unsigned>(GRID_SIZE);
}

inline int cellOf(int row, int col) noexcept {
    return row * GRID_SIZE + col;
}

inline int encoded(const Position& position) noexcept {
    return inside(position.row, position.col)
               ? cellOf(position.row, position.col)
               : -1;
}

inline int absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

Blockers makeBlockers(const GameInput& input) noexcept {
    Blockers result{encoded(input.visible_enemies[0]),
                    encoded(input.visible_enemies[1]), -1, -1};
    int count = input.num_visible_npcs;
    if (count < 3) return result;
    if (count > MAX_NPCS) count = MAX_NPCS;

    int cells[MAX_NPCS];
    int valid = 0;
    for (int index = 0; index < count; ++index) {
        const NpcInfo& npc = input.visible_npcs[index];
        if (npc.id != 0 && inside(npc.pos.row, npc.pos.col)) {
            cells[valid++] = cellOf(npc.pos.row, npc.pos.col);
        }
    }
    for (int index = 0; index < valid; ++index) {
        const int candidate = cells[index];
        if (candidate == result.crowded0) continue;
        int matches = 1;
        for (int next = index + 1; next < valid; ++next) {
            matches += cells[next] == candidate;
        }
        if (matches < 3) continue;
        if (result.crowded0 < 0) {
            result.crowded0 = candidate;
        } else {
            result.crowded1 = candidate;
            break;
        }
    }
    return result;
}

inline bool openCell(const GameInput& input, int row, int col, int ownBlock,
                     const Blockers& blockers) noexcept {
    if (!inside(row, col)) return false;
    const int cell = cellOf(row, col);
    return cell != ownBlock && cell != blockers.enemy0 &&
           cell != blockers.enemy1 && cell != blockers.crowded0 &&
           cell != blockers.crowded1 && input.grid[row][col] >= 0;
}

bool pressured(const GameInput& input, int row, int col) noexcept {
    for (const Position enemy : input.visible_enemies) {
        if (inside(enemy.row, enemy.col) &&
            absInt(enemy.row - row) + absInt(enemy.col - col) <= 1) {
            return true;
        }
    }
    return false;
}

inline int discountedValue(int value) noexcept {
    if (value > 1000000) value = 1000000;
    return (value * 7) / 20;
}

Target scalarScan(const GameInput& input, Position start, int radius,
                  int ownBlock, const Blockers& blockers,
                  bool contestAware) noexcept {
    Target best{-1, -1, 0, 0, INT_MAX};
    for (int dr = -radius; dr <= radius; ++dr) {
        const int row = start.row + dr;
        if (static_cast<unsigned>(row) >=
            static_cast<unsigned>(GRID_SIZE)) {
            continue;
        }
        for (int dc = -radius; dc <= radius; ++dc) {
            const int col = start.col + dc;
            if (static_cast<unsigned>(col) >=
                static_cast<unsigned>(GRID_SIZE)) {
                continue;
            }
            const int cell = cellOf(row, col);
            if (cell == ownBlock || cell == blockers.enemy0 ||
                cell == blockers.enemy1 || cell == blockers.crowded0 ||
                cell == blockers.crowded1) {
                continue;
            }
            int value = input.grid[row][col];
            if (value <= 0) continue;
            if (value > 1000000) value = 1000000;
            const int utility = contestAware && pressured(input, row, col)
                                    ? discountedValue(value)
                                    : value;
            const int distance = absInt(dr) + absInt(dc);
            if (utility > best.utility ||
                (utility == best.utility && distance < best.distance)) {
                best = Target{row, col, value, utility, distance};
            }
        }
    }
    return best;
}

Target scanMain(const GameInput& input, Position start, int ownBlock,
                const Blockers& blockers) noexcept {
    if (static_cast<unsigned>(start.row - 2) >= 13U ||
        static_cast<unsigned>(start.col - 2) >= 13U) {
        return scalarScan(input, start, 2, ownBlock, blockers, true);
    }

    const __m256i mask =
        _mm256_setr_epi32(-1, -1, -1, -1, -1, 0, 0, 0);
    __m256i best = _mm256_setzero_si256();
    __m256i bestRow = _mm256_setzero_si256();

#define SCAN_ROW(OFFSET)                                                     \
    do {                                                                     \
        const __m256i values = _mm256_maskload_epi32(                        \
            &input.grid[start.row + (OFFSET)][start.col - 2], mask);          \
        const __m256i better = _mm256_cmpgt_epi32(values, best);              \
        best = _mm256_blendv_epi8(best, values, better);                      \
        bestRow = _mm256_blendv_epi8(                                        \
            bestRow, _mm256_set1_epi32(start.row + (OFFSET)), better);        \
    } while (false)

    SCAN_ROW(-2);
    SCAN_ROW(-1);
    SCAN_ROW(0);
    SCAN_ROW(1);
    SCAN_ROW(2);
#undef SCAN_ROW

    alignas(32) int values[8];
    alignas(32) int rows[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(values), best);
    _mm256_store_si256(reinterpret_cast<__m256i*>(rows), bestRow);
    int column = 0;
    if (values[1] > values[column]) column = 1;
    if (values[2] > values[column]) column = 2;
    if (values[3] > values[column]) column = 3;
    if (values[4] > values[column]) column = 4;
    if (values[column] <= 0) return Target{-1, -1, 0, 0, INT_MAX};

    const int row = rows[column];
    const int col = start.col - 2 + column;
    const int cell = cellOf(row, col);
    if (cell == ownBlock || cell == blockers.enemy0 ||
        cell == blockers.enemy1 || cell == blockers.crowded0 ||
        cell == blockers.crowded1 || pressured(input, row, col)) {
        return scalarScan(input, start, 2, ownBlock, blockers, true);
    }
    return Target{row, col, values[column], values[column],
                  absInt(row - start.row) + absInt(col - start.col)};
}

int centerAction(const GameInput& input, Position position, int ownBlock,
                 const Blockers& blockers, int phase) noexcept {
    int bestAction = kStay;
    int bestScore = INT_MAX;
    for (int action = 0; action < 4; ++action) {
        const int row = position.row + kDr[action];
        const int col = position.col + kDc[action];
        if (!openCell(input, row, col, ownBlock, blockers)) continue;
        const int distance = absInt(row - kCenter) + absInt(col - kCenter);
        const int score = distance * 4 + ((action - phase) & 3);
        if (score < bestScore) {
            bestScore = score;
            bestAction = action;
        }
    }
    return bestAction;
}

int phaseAction(const GameInput& input, Position position, int ownBlock,
                const Blockers& blockers, int phase) noexcept {
    int preferred = kStay;
    if (position.row == 7 && position.col < 9) preferred = 3;
    else if (position.col == 9 && position.row < 9) preferred = 1;
    else if (position.row == 9 && position.col > 7) preferred = 2;
    else if (position.col == 7 && position.row > 7) preferred = 0;
    if (preferred != kStay &&
        openCell(input, position.row + kDr[preferred],
                 position.col + kDc[preferred], ownBlock, blockers)) {
        return preferred;
    }
    return centerAction(input, position, ownBlock, blockers, phase);
}

int directAction(const GameInput& input, Position position,
                 const Target& target, int ownBlock,
                 const Blockers& blockers, int phase) noexcept {
    const int vertical = target.row < position.row
                             ? 0
                             : (target.row > position.row ? 1 : kStay);
    const int horizontal = target.col < position.col
                               ? 2
                               : (target.col > position.col ? 3 : kStay);
    int first = vertical;
    int second = horizontal;
    if (first == kStay ||
        (second != kStay && ((phase ^ position.row ^ position.col) & 1))) {
        first = horizontal;
        second = vertical;
    }
    if (first != kStay &&
        openCell(input, position.row + kDr[first],
                 position.col + kDc[first], ownBlock, blockers)) {
        return first;
    }
    if (second != kStay &&
        openCell(input, position.row + kDr[second],
                 position.col + kDc[second], ownBlock, blockers)) {
        return second;
    }
    return kStay;
}

bool simulateWithBlock(const GameInput& input, Position position,
                       const int actions[S], int budget, int ownBlock,
                       const Blockers& blockers,
                       int dynamicBlock) noexcept {
    for (int step = 0; step < budget; ++step) {
        const int action = actions[step];
        if (action == kStay) continue;
        const int row = position.row + kDr[action];
        const int col = position.col + kDc[action];
        if (!inside(row, col)) return false;
        if (cellOf(row, col) == dynamicBlock) continue;
        if (!openCell(input, row, col, ownBlock, blockers)) return false;
        position = Position{row, col};
    }
    return true;
}

bool staticBlockSafe(const GameInput& input, Position start,
                     const int actions[S], int budget, int ownBlock,
                     const Blockers& blockers) noexcept {
    if (!simulateWithBlock(input, start, actions, budget, ownBlock,
                           blockers, -1)) {
        return false;
    }

    int states[64] = {cellOf(start.row, start.col)};
    int stateCount = 1;
    int candidates[64];
    int candidateCount = 0;
    for (int step = 0; step < budget; ++step) {
        const int action = actions[step];
        if (action == kStay) continue;
        int nextStates[64];
        int nextCount = 0;
        for (int index = 0; index < stateCount; ++index) {
            const int row = states[index] / GRID_SIZE;
            const int col = states[index] - row * GRID_SIZE;
            const int nextRow = row + kDr[action];
            const int nextCol = col + kDc[action];
            if (inside(nextRow, nextCol)) {
                const int attempted = cellOf(nextRow, nextCol);
                bool known = false;
                for (int old = 0; old < candidateCount; ++old) {
                    known |= candidates[old] == attempted;
                }
                if (!known) candidates[candidateCount++] = attempted;
            }
            const int alternatives[2] = {
                states[index], inside(nextRow, nextCol)
                                   ? cellOf(nextRow, nextCol)
                                   : states[index]};
            for (const int alternative : alternatives) {
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
    for (int index = 0; index < candidateCount; ++index) {
        if (!simulateWithBlock(input, start, actions, budget, ownBlock,
                               blockers, candidates[index])) {
            return false;
        }
    }
    return true;
}

Route straightFallback(const GameInput& input, Position start,
                       const Target& target, int unit, int budget,
                       int ownBlock, const Blockers& blockers) noexcept {
    Route route{{kStay, kStay, kStay, kStay, kStay, kStay}, start};
    int preferred = phaseAction(input, start, ownBlock, blockers,
                                input.round + unit * 3);
    if (target.value > 0) {
        if (target.row < start.row) preferred = 0;
        else if (target.row > start.row) preferred = 1;
        else if (target.col < start.col) preferred = 2;
        else if (target.col > start.col) preferred = 3;
    }
    if (preferred == kStay) {
        return route;
    }
    static constexpr int kTurns[4][4] = {
        {0, 3, 2, 1}, {1, 2, 3, 0}, {2, 0, 1, 3}, {3, 1, 0, 2}};
    for (int choice = 0; choice < 4; ++choice) {
        const int action = kTurns[preferred][choice];
        int length = 0;
        while (length < budget &&
               openCell(input, start.row + (length + 1) * kDr[action],
                        start.col + (length + 1) * kDc[action], ownBlock,
                        blockers)) {
            ++length;
        }
        if (length == 0) continue;
        for (int step = 0; step < length; ++step) {
            route.actions[step] = action;
        }
        route.end.row += length * kDr[action];
        route.end.col += length * kDc[action];
        break;
    }
    return route;
}

Route planRoute(const GameInput& input, Position start, const Target& target,
                int unit, int budget, int ownBlock,
                const Blockers& blockers) noexcept {
    Route route{{kStay, kStay, kStay, kStay, kStay, kStay}, start};
    Position position = start;
    int used = 0;
    int previous = kStay;
    while (used < budget && target.value > 0 &&
           (position.row != target.row || position.col != target.col)) {
        const int action = directAction(input, position, target, ownBlock,
                                        blockers, input.round + used + unit);
        if (action == kStay) break;
        route.actions[used++] = action;
        previous = action;
        position.row += kDr[action];
        position.col += kDc[action];
    }

    if (target.value > 0 && position.row == target.row &&
        position.col == target.col) {
        while (used + 1 < budget) {
            const int away = previous == kStay
                                 ? phaseAction(input, position, ownBlock,
                                               blockers,
                                               input.round + used + unit)
                                 : (previous ^ 1);
            if (away == kStay ||
                !openCell(input, position.row + kDr[away],
                          position.col + kDc[away], ownBlock, blockers)) {
                break;
            }
            route.actions[used++] = away;
            position.row += kDr[away];
            position.col += kDc[away];
            route.actions[used++] = away ^ 1;
            position.row -= kDr[away];
            position.col -= kDc[away];
        }
    } else if (target.value <= 0) {
        const int movementBudget = budget < 3 ? budget : 3;
        while (used < movementBudget) {
            const int action = phaseAction(input, position, ownBlock,
                                           blockers,
                                           input.round + used + unit * 3);
            route.actions[used++] = action;
            if (action != kStay) {
                position.row += kDr[action];
                position.col += kDc[action];
            }
        }
    }
    route.end = position;
    bool contested = target.value > 0 &&
                     pressured(input, target.row, target.col);
    for (const Position enemy : input.visible_enemies) {
        contested |= inside(enemy.row, enemy.col) &&
                     absInt(enemy.row - start.row) +
                             absInt(enemy.col - start.col) <=
                         2;
    }
    if (contested &&
        !staticBlockSafe(input, start, route.actions, budget, ownBlock,
                         blockers)) {
        route = straightFallback(input, start, target, unit, budget,
                                 ownBlock, blockers);
    }
    return route;
}

GameOutput fallback() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

GameOutput decide(const GameInput& input) noexcept {
    const Blockers blockers = makeBlockers(input);
    const Position guardStart = input.my_units[0];
    const Position mainStart = input.my_units[1];
    Target guard = scalarScan(input, guardStart, 1, encoded(mainStart),
                              blockers, true);
    Target main = scanMain(input, mainStart, encoded(guardStart), blockers);

    int allocation = 0;
    if (guard.value > 0) {
        allocation = guard.distance == 1 ? 3 : 2;
    } else if (guardStart.row != kCenter || guardStart.col != kCenter) {
        allocation = 2;
    } else if (main.value <= 0 && input.round % 3 == 0) {
        allocation = 2;
    }
    const int mainBudget = S - allocation;
    const int order = allocation > 0 && guard.utility > main.utility ? 0 : 1;

    Route guardRoute{{kStay, kStay, kStay, kStay, kStay, kStay},
                     guardStart};
    Route mainRoute{{kStay, kStay, kStay, kStay, kStay, kStay},
                    mainStart};
    if (order == 0) {
        guardRoute = planRoute(input, guardStart, guard, 0, allocation,
                               encoded(mainStart), blockers);
        if (main.value > 0 &&
            cellOf(main.row, main.col) == encoded(guardRoute.end)) {
            main = scalarScan(input, mainStart, 2, encoded(guardRoute.end),
                              blockers, true);
        }
        mainRoute = planRoute(input, mainStart, main, 1, mainBudget,
                              encoded(guardRoute.end), blockers);
    } else {
        mainRoute = planRoute(input, mainStart, main, 1, mainBudget,
                              encoded(guardStart), blockers);
        if (guard.value > 0 &&
            cellOf(guard.row, guard.col) == encoded(mainRoute.end)) {
            guard = scalarScan(input, guardStart, 1,
                               encoded(mainRoute.end), blockers, true);
        }
        guardRoute = planRoute(input, guardStart, guard, 0, allocation,
                               encoded(mainRoute.end), blockers);
    }

    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay},
                      allocation, order, 0};
    for (int step = 0; step < allocation; ++step) {
        output.actions[step] = guardRoute.actions[step];
    }
    for (int step = 0; step < mainBudget; ++step) {
        output.actions[allocation + step] = mainRoute.actions[step];
    }
    return output;
}

}  // namespace

extern "C" __attribute__((visibility("default")))
GameOutput moveDecision(const GameInput* input) {
    if (input == nullptr || input->round < 0 || input->round > 1000000 ||
        !inside(input->my_units[0].row, input->my_units[0].col) ||
        !inside(input->my_units[1].row, input->my_units[1].col) ||
        (input->my_units[0].row == input->my_units[1].row &&
         input->my_units[0].col == input->my_units[1].col)) {
        return fallback();
    }
    return decide(*input);
}
