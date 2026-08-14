#include "game_api.h"

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define HOT_INLINE __attribute__((always_inline)) inline
#else
#define HOT_INLINE inline
#endif

constexpr int kStay = 4;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

struct Probe {
    int action;
    int value;
};

struct UnitPlan {
    int actions[3];
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

inline bool traversable(const GameInput& input, int row, int col,
                        int block0, int block1, int block2) noexcept {
    if (!inside(row, col)) return false;
    const int cell = cellOf(row, col);
    return cell != block0 && cell != block1 && cell != block2 &&
           input.grid[row][col] >= 0;
}

HOT_INLINE int phaseDirection(const Position& position, int unit,
                              int round) noexcept {
    const int low = unit == 0 ? 6 : 5;
    const int high = unit == 0 ? 10 : 11;
    if (position.row < low) return 1;
    if (position.row > high) return 0;
    if (position.col < low) return 3;
    if (position.col > high) return 2;

    if (unit == 0) {
        if (position.row == low && position.col < high) return 3;
        if (position.col == high && position.row < high) return 1;
        if (position.row == high && position.col > low) return 2;
        if (position.col == low && position.row > low) return 0;
    } else {
        if (position.row == low && position.col > low) return 2;
        if (position.col == low && position.row < high) return 1;
        if (position.row == high && position.col < high) return 3;
        if (position.col == high && position.row > low) return 0;
    }
    static constexpr int kInterior[8] = {3, 1, 2, 0, 2, 1, 3, 0};
    return kInterior[(round + unit * 3) & 7];
}

HOT_INLINE Probe probeGold(const GameInput& input, Position position,
                           int block0, int block1,
                           int block2) noexcept {
    Probe result{kStay, input.grid[position.row][position.col]};
    for (int action = 0; action < 4; ++action) {
        const int row = position.row + kDr[action];
        const int col = position.col + kDc[action];
        if (!traversable(input, row, col, block0, block1, block2)) continue;
        const int value = input.grid[row][col];
        if (value > result.value) {
            result = Probe{action, value};
        }
    }
    return result;
}

HOT_INLINE bool bounceSafe(const GameInput& input, Position position,
                           int action, int block0, int block1,
                           int block2) noexcept {
    return traversable(input, position.row + kDr[action],
                       position.col + kDc[action], block0, block1, block2) &&
           traversable(input, position.row - kDr[action],
                       position.col - kDc[action], block0, block1, block2) &&
           traversable(input, position.row + 2 * kDr[action],
                       position.col + 2 * kDc[action], block0, block1,
                       block2);
}

HOT_INLINE UnitPlan straightPlan(const GameInput& input, int unit,
                                 int preferred, int block0, int block1,
                                 int block2) noexcept {
    const Position start = input.my_units[unit];
    UnitPlan result{{kStay, kStay, kStay}, start};
    static constexpr int kTurns[4][4] = {
        {0, 3, 2, 1}, {1, 2, 3, 0}, {2, 0, 1, 3}, {3, 1, 0, 2}};
    int bestDirection = kStay;
    int bestLength = 0;
    for (int choice = 0; choice < 4; ++choice) {
        const int action = kTurns[preferred][choice];
        int length = 0;
        for (int step = 1; step <= 3; ++step) {
            if (!traversable(input, start.row + step * kDr[action],
                             start.col + step * kDc[action], block0, block1,
                             block2)) {
                break;
            }
            ++length;
        }
        if (length > 0) {
            bestLength = length;
            bestDirection = action;
            break;
        }
    }
    for (int step = 0; step < bestLength; ++step) {
        result.actions[step] = bestDirection;
    }
    if (bestDirection != kStay) {
        result.end.row += bestLength * kDr[bestDirection];
        result.end.col += bestLength * kDc[bestDirection];
    }
    return result;
}

HOT_INLINE UnitPlan planUnit(const GameInput& input, int unit, int preferred,
                             Probe probe, int block0, int block1,
                             int block2) noexcept {
    const Position start = input.my_units[unit];
    int action = probe.action;
    if (probe.value > 0) {
        if (action == kStay) {
            action = preferred;
        }
        if (action != kStay &&
            bounceSafe(input, start, action, block0, block1, block2)) {
            return UnitPlan{{action, action ^ 1, action},
                            Position{start.row + kDr[action],
                                     start.col + kDc[action]}};
        }
    }
    const int direction = action != kStay ? action : preferred;
    return straightPlan(input, unit, direction, block0, block1, block2);
}

GameOutput fallback() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

GameOutput decide(const GameInput& input) noexcept {
    const int enemy0 = encoded(input.visible_enemies[0]);
    const int enemy1 = encoded(input.visible_enemies[1]);
    const int cell0 = encoded(input.my_units[0]);
    const int cell1 = encoded(input.my_units[1]);
    const int phase0 = phaseDirection(input.my_units[0], 0, input.round);
    const int phase1 = phaseDirection(input.my_units[1], 1, input.round);
    const Probe priority0 = probeGold(input, input.my_units[0], cell1,
                                      enemy0, enemy1);
    const Probe priority1 = probeGold(input, input.my_units[1], cell0,
                                      enemy0, enemy1);
    const UnitPlan mainPlan = planUnit(input, 1, phase1, priority1, cell0,
                                       enemy0, enemy1);
    const UnitPlan guardPlan = planUnit(input, 0, phase0, priority0,
                                        encoded(mainPlan.end), enemy0,
                                        enemy1);

    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay},
                      3, 1, 0};
    for (int index = 0; index < 3; ++index) {
        output.actions[index] = guardPlan.actions[index];
        output.actions[index + 3] = mainPlan.actions[index];
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
