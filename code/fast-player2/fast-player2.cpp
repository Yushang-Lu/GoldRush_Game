#include "game_api.h"

#include <climits>
#include <immintrin.h>

static_assert(sizeof(int) == 4, "GoldRush ABI requires 32-bit int");
static_assert(sizeof(GameInput) == 1444, "GameInput ABI mismatch");
static_assert(sizeof(GameOutput) == 36, "GameOutput ABI mismatch");

namespace {

constexpr int kSide = GRID_SIZE;
constexpr int kStay = 4;
constexpr int kCenter = 8;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

inline bool inside(int row, int col) noexcept {
    return static_cast<unsigned>(row) < static_cast<unsigned>(kSide) &&
           static_cast<unsigned>(col) < static_cast<unsigned>(kSide);
}

inline int cellOf(int row, int col) noexcept {
    return row * kSide + col;
}

inline int encodedPosition(const Position& position) noexcept {
    return inside(position.row, position.col)
               ? cellOf(position.row, position.col)
               : -1;
}

inline bool safeCell(const GameInput& input, int row, int col, int block0,
                     int block1, int block2) noexcept {
    if (!inside(row, col)) return false;
    const int cell = cellOf(row, col);
    return cell != block0 && cell != block1 && cell != block2 &&
           input.grid[row][col] >= 0;
}

inline int absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

int centerStep(const GameInput& input, int row, int col, int block0,
               int block1, int block2, int phase) noexcept {
    int bestAction = kStay;
    int bestScore = INT_MAX;
    for (int action = 0; action < 4; ++action) {
        const int nextRow = row + kDr[action];
        const int nextCol = col + kDc[action];
        if (!safeCell(input, nextRow, nextCol, block0, block1, block2)) {
            continue;
        }
        const int distance = absInt(nextRow - kCenter) +
                             absInt(nextCol - kCenter);
        const int tie = (action - phase) & 3;
        const int score = distance * 4 + tie;
        if (score < bestScore) {
            bestScore = score;
            bestAction = action;
        }
    }
    return bestAction;
}

int ringAction(int row, int col, int phase) noexcept {
    if (row == 7 && col < 9) return 3;
    if (col == 9 && row < 9) return 1;
    if (row == 9 && col > 7) return 2;
    if (col == 7 && row > 7) return 0;
    static constexpr int fromCenter[4] = {0, 3, 1, 2};
    return fromCenter[phase & 3];
}

int ringStep(const GameInput& input, int row, int col, int block0,
             int block1, int block2, int phase) noexcept {
    const int preferred = ringAction(row, col, phase);
    if (safeCell(input, row + kDr[preferred], col + kDc[preferred], block0,
                 block1, block2)) {
        return preferred;
    }
    return centerStep(input, row, col, block0, block1, block2, phase);
}

struct GoldTarget {
    int row;
    int col;
    int value;
};

GoldTarget scalarScan25(const GameInput& input, int centerRow, int centerCol,
                        int block0, int block1, int block2) noexcept {
    GoldTarget best{-1, -1, 0};
    for (int dr = -2; dr <= 2; ++dr) {
        const int row = centerRow + dr;
        if (static_cast<unsigned>(row) >= static_cast<unsigned>(kSide)) {
            continue;
        }
        for (int dc = -2; dc <= 2; ++dc) {
            const int col = centerCol + dc;
            if (static_cast<unsigned>(col) >= static_cast<unsigned>(kSide)) {
                continue;
            }
            const int cell = cellOf(row, col);
            const int value = input.grid[row][col];
            if (cell != block0 && cell != block1 && cell != block2 &&
                value > best.value) {
                best = GoldTarget{row, col, value};
            }
        }
    }
    return best;
}

GoldTarget scalarScan9(const GameInput& input, int centerRow, int centerCol,
                       int block0, int block1, int block2) noexcept {
    GoldTarget best{-1, -1, 0};
    for (int dr = -1; dr <= 1; ++dr) {
        const int row = centerRow + dr;
        if (static_cast<unsigned>(row) >= static_cast<unsigned>(kSide)) {
            continue;
        }
        for (int dc = -1; dc <= 1; ++dc) {
            const int col = centerCol + dc;
            if (static_cast<unsigned>(col) >= static_cast<unsigned>(kSide)) {
                continue;
            }
            const int cell = cellOf(row, col);
            const int value = input.grid[row][col];
            if (cell != block0 && cell != block1 && cell != block2 &&
                value > best.value) {
                best = GoldTarget{row, col, value};
            }
        }
    }
    return best;
}

GoldTarget vectorScan25(const GameInput& input, int centerRow, int centerCol,
                        int block0, int block1, int block2) noexcept {
    if (static_cast<unsigned>(centerRow - 2) >= 13U ||
        static_cast<unsigned>(centerCol - 2) >= 13U) {
        return scalarScan25(input, centerRow, centerCol, block0, block1,
                            block2);
    }

    const __m256i loadMask =
        _mm256_setr_epi32(-1, -1, -1, -1, -1, 0, 0, 0);
    __m256i best = _mm256_setzero_si256();
    __m256i bestRow = _mm256_setzero_si256();

#define SCAN_ROW(OFFSET)                                                     \
    do {                                                                     \
        const __m256i values = _mm256_maskload_epi32(                        \
            &input.grid[centerRow + (OFFSET)][centerCol - 2], loadMask);      \
        const __m256i better = _mm256_cmpgt_epi32(values, best);              \
        best = _mm256_blendv_epi8(best, values, better);                      \
        bestRow = _mm256_blendv_epi8(                                        \
            bestRow, _mm256_set1_epi32(centerRow + (OFFSET)), better);        \
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
    int columnIndex = 0;
    if (values[1] > values[columnIndex]) columnIndex = 1;
    if (values[2] > values[columnIndex]) columnIndex = 2;
    if (values[3] > values[columnIndex]) columnIndex = 3;
    if (values[4] > values[columnIndex]) columnIndex = 4;
    if (values[columnIndex] <= 0) return GoldTarget{-1, -1, 0};

    const int row = rows[columnIndex];
    const int col = centerCol - 2 + columnIndex;
    const int cell = cellOf(row, col);
    if (cell == block0 || cell == block1 || cell == block2) {
        return scalarScan25(input, centerRow, centerCol, block0, block1,
                            block2);
    }
    return GoldTarget{row, col, values[columnIndex]};
}

int directStep(const GameInput& input, Position position,
               const GoldTarget& target, int block0, int block1,
               int block2, int phase) noexcept {
    const int vertical = target.row < position.row
                             ? 0
                             : (target.row > position.row ? 1 : kStay);
    const int horizontal = target.col < position.col
                               ? 2
                               : (target.col > position.col ? 3 : kStay);
    int first = vertical;
    int second = horizontal;
    if (vertical == kStay ||
        (horizontal != kStay && ((phase ^ position.row ^ position.col) & 1))) {
        first = horizontal;
        second = vertical;
    }
    const bool firstSafe =
        first != kStay &&
        safeCell(input, position.row + kDr[first], position.col + kDc[first],
                 block0, block1, block2);
    const bool secondSafe =
        second != kStay &&
        safeCell(input, position.row + kDr[second],
                 position.col + kDc[second], block0, block1, block2);
    if (firstSafe && secondSafe &&
        input.grid[position.row + kDr[second]][position.col + kDc[second]] >
            input.grid[position.row + kDr[first]][position.col + kDc[first]]) {
        return second;
    }
    if (firstSafe) return first;
    if (secondSafe) return second;
    return kStay;
}

Position planMain(const GameInput& input, GameOutput& output, int begin,
                  int guardCell, int enemy0, int enemy1) noexcept {
    Position position = input.my_units[1];
    const int budget = S - begin;
    const GoldTarget target = vectorScan25(
        input, position.row, position.col, guardCell, enemy0, enemy1);

    int used = 0;
    int previous = kStay;
    while (used < budget && target.value > 0 &&
           (position.row != target.row || position.col != target.col)) {
        const int action = directStep(input, position, target, guardCell,
                                      enemy0, enemy1, input.round + used);
        if (action == kStay) break;
        output.actions[begin + used] = action;
        ++used;
        previous = action;
        position.row += kDr[action];
        position.col += kDc[action];
    }

    if (target.value > 0 && position.row == target.row &&
        position.col == target.col) {
        int away = previous == kStay
                       ? ringStep(input, position.row, position.col, guardCell,
                                  enemy0, enemy1, input.round + used)
                       : (previous ^ 1);
        if (away != kStay && used + 1 < budget &&
            safeCell(input, position.row + kDr[away],
                     position.col + kDc[away], guardCell, enemy0, enemy1)) {
            while (used + 1 < budget) {
                output.actions[begin + used] = away;
                output.actions[begin + used + 1] = away ^ 1;
                used += 2;
            }
        }
        return position;
    }
    if (target.value > 0) return position;

    const bool inCore = static_cast<unsigned>(position.row - 7) < 3U &&
                        static_cast<unsigned>(position.col - 7) < 3U;
    if (!inCore) {
        const int moves = budget < 2 ? budget : 2;
        for (int step = 0; step < moves; ++step) {
            const int action = centerStep(
                input, position.row, position.col, guardCell, enemy0, enemy1,
                input.round + step);
            output.actions[begin + step] = action;
            if (action != kStay) {
                position.row += kDr[action];
                position.col += kDc[action];
            }
        }
        return position;
    }

    const int moves = budget == S ? 3 : 2;
    for (int step = 0; step < moves; ++step) {
        const int action = ringStep(input, position.row, position.col,
                                    guardCell, enemy0, enemy1,
                                    input.round + step);
        output.actions[begin + step] = action;
        if (action != kStay) {
            position.row += kDr[action];
            position.col += kDc[action];
        }
    }
    return position;
}

void planGuard(const GameInput& input, GameOutput& output, int mainEnd,
               int enemy0, int enemy1, GoldTarget target) noexcept {
    Position position = input.my_units[0];
    if (target.value > 0 && cellOf(target.row, target.col) == mainEnd) {
        target = scalarScan9(input, position.row, position.col, mainEnd,
                             enemy0, enemy1);
    }
    if (target.value > 0) {
        int used = 0;
        int previous = kStay;
        while (used < output.k &&
               (position.row != target.row || position.col != target.col)) {
            const int action = directStep(input, position, target, mainEnd,
                                          enemy0, enemy1,
                                          input.round + used + 2);
            if (action == kStay) break;
            output.actions[used++] = action;
            previous = action;
            position.row += kDr[action];
            position.col += kDc[action];
        }
        if (position.row == target.row && position.col == target.col) {
            const int away = previous == kStay
                                 ? ringStep(input, position.row, position.col,
                                            mainEnd, enemy0, enemy1,
                                            input.round + used + 2)
                                 : (previous ^ 1);
            if (away != kStay && used + 1 < output.k &&
                safeCell(input, position.row + kDr[away],
                         position.col + kDc[away], mainEnd, enemy0, enemy1)) {
                output.actions[used] = away;
                output.actions[used + 1] = away ^ 1;
            }
        }
        return;
    }

    if (position.row != kCenter || position.col != kCenter) {
        for (int step = 0; step < output.k; ++step) {
            const int action = centerStep(
                input, position.row, position.col, mainEnd, enemy0, enemy1,
                input.round + step + 2);
            output.actions[step] = action;
            if (action != kStay) {
                position.row += kDr[action];
                position.col += kDc[action];
            }
        }
        return;
    }
}

GameOutput fallbackOutput() noexcept {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 0, 0, 0};
}

GameOutput decide(const GameInput& input) noexcept {
    const Position guard = input.my_units[0];
    const bool guardHome = guard.row == kCenter && guard.col == kCenter;
    const int guardCell = cellOf(guard.row, guard.col);
    const int enemy0 = encodedPosition(input.visible_enemies[0]);
    const int enemy1 = encodedPosition(input.visible_enemies[1]);
    GoldTarget guardTarget = scalarScan9(
        input, guard.row, guard.col, cellOf(input.my_units[1].row,
                                            input.my_units[1].col),
        enemy0, enemy1);
    int allocation = 0;
    if (guardTarget.value > 0) {
        const int distance = absInt(guard.row - guardTarget.row) +
                             absInt(guard.col - guardTarget.col);
        allocation = distance == 1 ? 3 : 2;
    } else if (!guardHome) {
        allocation = 2;
    }
    GameOutput output{{kStay, kStay, kStay, kStay, kStay, kStay},
                      allocation, 1, 0};

    const Position mainEnd = planMain(input, output, allocation, guardCell,
                                      enemy0, enemy1);
    planGuard(input, output, cellOf(mainEnd.row, mainEnd.col), enemy0, enemy1,
              guardTarget);
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
        return fallbackOutput();
    }
    return decide(*input);
}
