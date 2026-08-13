#include "game_api.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr int kCells = GRID_SIZE * GRID_SIZE;
constexpr int kWords = (kCells + 31) / 32;
constexpr int kStay = 4;
constexpr int kBadScore = -1000000000;
constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

inline int cellIndex(int row, int col) noexcept {
    return row * GRID_SIZE + col;
}

struct Tables {
    std::int16_t neighbor[kCells][4];
    std::uint8_t row[kCells];
    std::uint8_t col[kCells];
    std::uint8_t region[kCells];
};

Tables makeTables() noexcept {
    Tables result{};
    for (int cell = 0; cell < kCells; ++cell) {
        const int row = cell / GRID_SIZE;
        const int col = cell % GRID_SIZE;
        result.row[cell] = static_cast<std::uint8_t>(row);
        result.col[cell] = static_cast<std::uint8_t>(col);
        if (row >= 4 && row <= 12 && col >= 4 && col <= 12) {
            result.region[cell] = 1;
        } else if (row <= 3 && col <= 12) {
            result.region[cell] = 2;
        } else if (col <= 3 && row >= 4) {
            result.region[cell] = 3;
        } else if (row >= 13 && col >= 4) {
            result.region[cell] = 4;
        } else {
            result.region[cell] = 5;
        }
        for (int action = 0; action < 4; ++action) {
            const int next_row = row + kDr[action];
            const int next_col = col + kDc[action];
            result.neighbor[cell][action] =
                next_row >= 0 && next_row < GRID_SIZE && next_col >= 0 &&
                        next_col < GRID_SIZE
                    ? static_cast<std::int16_t>(
                          cellIndex(next_row, next_col))
                    : static_cast<std::int16_t>(-1);
        }
    }
    return result;
}

const Tables g_tables = makeTables();

inline int rowOf(int cell) noexcept {
    return g_tables.row[cell];
}

inline int colOf(int cell) noexcept {
    return g_tables.col[cell];
}

inline int adjacentCell(int cell, int action) noexcept {
    return g_tables.neighbor[cell][action];
}

inline int absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

inline int manhattan(int lhs, int rhs) noexcept {
    return absInt(rowOf(lhs) - rowOf(rhs)) +
           absInt(colOf(lhs) - colOf(rhs));
}

inline bool validPosition(const Position& position) noexcept {
    return static_cast<unsigned>(position.row) < GRID_SIZE &&
           static_cast<unsigned>(position.col) < GRID_SIZE;
}

inline int clampNonnegative(int value, int maximum) noexcept {
    if (value <= 0) {
        return 0;
    }
    return value < maximum ? value : maximum;
}

inline int pickupAmount(int gold) noexcept {
    return gold <= 0 ? 0 : static_cast<int>(
        (static_cast<std::int64_t>(gold) * 65 + 99) / 100);
}

inline void setBit(std::uint32_t* bits, int cell) noexcept {
    bits[cell >> 5] |= std::uint32_t{1} << (cell & 31);
}

inline void clearBit(std::uint32_t* bits, int cell) noexcept {
    bits[cell >> 5] &= ~(std::uint32_t{1} << (cell & 31));
}

inline bool testBit(const std::uint32_t* bits, int cell) noexcept {
    return (bits[cell >> 5] &
            (std::uint32_t{1} << (cell & 31))) != 0;
}

struct PersistentState {
    bool initialized;
    int last_round;
    int last_input_signature;
    int last_purchase_round;
    int vision_spend;
    int active_bomb_cycle;
    std::uint32_t walls[kWords];
    std::uint32_t bombs[kWords];
    int last_seen_round[kCells];
    int last_visit_round[kCells];
    int region_score[REGION_COUNT + 1];
};

PersistentState g_state{};

struct TurnContext {
    const GameInput* input;
    const int* grid;
    int gold_cells[kCells];
    int gold_count;
    std::uint32_t forbidden[kWords];
    int visible_gold_sum;
    int bomb_cycle;
};

struct Route {
    std::uint8_t actions[S];
};

GameOutput fallbackOutput() noexcept {
    GameOutput output;
    for (int step = 0; step < S; ++step) {
        output.actions[step] = kStay;
    }
    output.k = 0;
    output.order = 0;
    output.vp = 0;
    return output;
}

void resetState() noexcept {
    g_state.initialized = true;
    g_state.last_round = -1;
    g_state.last_input_signature = 0;
    g_state.last_purchase_round = -1000;
    g_state.vision_spend = 0;
    g_state.active_bomb_cycle = 0;
    std::memset(g_state.walls, 0, sizeof(g_state.walls));
    std::memset(g_state.bombs, 0, sizeof(g_state.bombs));
    std::memset(g_state.region_score, 0, sizeof(g_state.region_score));
    for (int cell = 0; cell < kCells; ++cell) {
        g_state.last_seen_round[cell] = -1000;
        g_state.last_visit_round[cell] = -1000;
    }
}

void updateSnapshot(const GameInput& input) noexcept {
    if (input.snapshot_valid != 1) {
        return;
    }
    for (int index = 0; index < REGION_COUNT; ++index) {
        const RegionStat& stat = input.snapshot.regions[index];
        if (stat.id < 1 || stat.id > REGION_COUNT) {
            continue;
        }
        const int area = stat.id == 1 ? 81 : 52;
        const int remaining =
            clampNonnegative(stat.gold_remaining, 1000000);
        const int generated =
            clampNonnegative(stat.gold_generated, 1000000);
        const int collected =
            clampNonnegative(stat.gold_collected, 1000000);
        const int occupants = clampNonnegative(stat.occupants, 100);
        const int flow = generated > collected ? generated - collected : 0;
        int score = (remaining * 8 + flow * 2) / area - occupants * 3;
        if (score < -40) {
            score = -40;
        } else if (score > 120) {
            score = 120;
        }
        g_state.region_score[stat.id] = score;
    }
}

void prepareState(const GameInput& input) noexcept {
    const int signature = cellIndex(input.my_units[0].row,
                                    input.my_units[0].col) * kCells +
                          cellIndex(input.my_units[1].row,
                                    input.my_units[1].col);
    const bool discontinuous = g_state.initialized &&
        g_state.last_round >= 0 &&
        (input.round != g_state.last_round + 1 ||
         (input.round == g_state.last_round &&
          signature != g_state.last_input_signature));
    if (!g_state.initialized || input.round == 0 || discontinuous) {
        resetState();
    }
    updateSnapshot(input);
    g_state.last_input_signature = signature;
}

void updateVisibleCell(const GameInput& input, TurnContext& context,
                       int cell) noexcept {
    const int value = context.grid[cell];
    if (value == -5) {
        return;
    }
    g_state.last_seen_round[cell] = input.round;
    if (value == -1) {
        setBit(g_state.walls, cell);
        clearBit(g_state.bombs, cell);
        return;
    }
    clearBit(g_state.walls, cell);
    if (value == -3) {
        setBit(g_state.bombs, cell);
        return;
    }
    clearBit(g_state.bombs, cell);
    const int gold = clampNonnegative(value, 1000000);
    if (gold > 0) {
        if (context.gold_count < kCells) {
            context.gold_cells[context.gold_count++] = cell;
        }
        context.visible_gold_sum += gold < 1000 ? gold : 1000;
    }
}

void buildContext(const GameInput& input, TurnContext& context) noexcept {
    context.input = &input;
    context.grid = &input.grid[0][0];
    context.visible_gold_sum = 0;
    context.gold_count = 0;
    context.bomb_cycle = input.round / 20 + 1;
    if (context.bomb_cycle != g_state.active_bomb_cycle) {
        std::memset(g_state.bombs, 0, sizeof(g_state.bombs));
        g_state.active_bomb_cycle = context.bomb_cycle;
    }
    for (int cell = 0; cell < kCells; ++cell) {
        if (context.grid[cell] != -5) {
            updateVisibleCell(input, context, cell);
        }
    }
    for (int word = 0; word < kWords; ++word) {
        context.forbidden[word] =
            g_state.walls[word] | g_state.bombs[word];
    }
    for (int enemy = 0; enemy < 2; ++enemy) {
        if (validPosition(input.visible_enemies[enemy])) {
            setBit(context.forbidden,
                   cellIndex(input.visible_enemies[enemy].row,
                             input.visible_enemies[enemy].col));
        }
    }

    int npc_cells[MAX_NPCS] = {};
    std::uint8_t npc_counts[MAX_NPCS] = {};
    int distinct_npcs = 0;
    int count = input.num_visible_npcs;
    if (count < 0) {
        count = 0;
    } else if (count > MAX_NPCS) {
        count = MAX_NPCS;
    }
    for (int index = 0; index < count; ++index) {
        const NpcInfo& npc = input.visible_npcs[index];
        if (npc.id == 0 || !validPosition(npc.pos)) {
            continue;
        }
        const int cell = cellIndex(npc.pos.row, npc.pos.col);
        int slot = 0;
        while (slot < distinct_npcs && npc_cells[slot] != cell) {
            ++slot;
        }
        if (slot == distinct_npcs && distinct_npcs < MAX_NPCS) {
            npc_cells[distinct_npcs] = cell;
            npc_counts[distinct_npcs] = 0;
            ++distinct_npcs;
        }
        if (++npc_counts[slot] >= 3) {
            setBit(context.forbidden, cell);
        }
    }
    g_state.last_round = input.round;
}

inline int estimatedGold(const TurnContext& context, int cell) noexcept {
    const int value = context.grid[cell];
    return value <= 0 ? 0 : (value < 1000000 ? value : 1000000);
}

int goldLeft(const TurnContext& context, int cell,
             const int (&gold_delta)[kCells]) noexcept {
    return gold_delta[cell] >= 0 ? gold_delta[cell] :
                                  estimatedGold(context, cell);
}

int collectGold(const TurnContext& context, int cell,
                int (&gold_delta)[kCells]) noexcept {
    const int remaining = goldLeft(context, cell, gold_delta);
    const int pickup = pickupAmount(remaining);
    if (pickup <= 0) {
        return 0;
    }
    gold_delta[cell] = remaining - pickup;
    return pickup;
}

int explorationValue(const TurnContext& context, int cell, int held,
                     int other) noexcept {
    int value = 40 - absInt(rowOf(cell) - 8) * 3 -
                absInt(colOf(cell) - 8) * 3;
    const int seen = g_state.last_seen_round[cell];
    const int seen_age = context.input->round - seen;
    value += seen < 0 ? 36 : (seen_age > 12 ? 24 : seen_age * 2);
    const int visited = g_state.last_visit_round[cell];
    const int visit_age = context.input->round - visited;
    value += visited < 0 ? 18 : (visit_age > 20 ? 12 : visit_age / 2);
    value += g_state.region_score[g_tables.region[cell]];
    int separation = manhattan(cell, other);
    if (separation > 8) {
        separation = 8;
    }
    value += separation * 3;
    if (context.grid[cell] == -5) {
        value += 10;
        if (held > 600) {
            value -= held > 1200 ? 28 : held / 45;
        }
    }
    return value;
}

std::int64_t evaluateJoint(const TurnContext& context, GameOutput& output,
                           int (&ends)[2]) noexcept {
    ends[0] = cellIndex(context.input->my_units[0].row,
                        context.input->my_units[0].col);
    ends[1] = cellIndex(context.input->my_units[1].row,
                        context.input->my_units[1].col);
    int held[2] = {
        clampNonnegative(context.input->my_units_gold[0], 1000000000),
        clampNonnegative(context.input->my_units_gold[1], 1000000000)};
    int changed_cells[S] = {};
    int gold_left[S] = {};
    int changed_count = 0;
    int pickup_total = 0;
    for (int unit = 0; unit < 2; ++unit) {
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) {
                continue;
            }
            const int next = adjacentCell(ends[unit], action);
            if (next < 0 || next == ends[1 - unit] ||
                testBit(context.forbidden, next)) {
                output.actions[index] = kStay;
                continue;
            }
            ends[unit] = next;
            int remaining = estimatedGold(context, next);
            int changed_index = -1;
            for (int changed = 0; changed < changed_count; ++changed) {
                if (changed_cells[changed] == next) {
                    remaining = gold_left[changed];
                    changed_index = changed;
                    break;
                }
            }
            const int pickup = pickupAmount(remaining);
            if (pickup > 0) {
                pickup_total += pickup;
                held[unit] += pickup;
                if (changed_index >= 0) {
                    gold_left[changed_index] = remaining - pickup;
                } else if (changed_count < S) {
                    changed_cells[changed_count] = next;
                    gold_left[changed_count] = remaining - pickup;
                    ++changed_count;
                }
            }
        }
    }
    std::int64_t score = static_cast<std::int64_t>(pickup_total) * 1800;
    score += explorationValue(context, ends[0], held[0], ends[1]);
    score += explorationValue(context, ends[1], held[1], ends[0]);
    int separation = manhattan(ends[0], ends[1]);
    if (separation > 8) {
        separation = 8;
    }
    return score + separation * 10;
}

Route buildFastRoute(const TurnContext& context, int start, int other,
                     int target, int initial_held) noexcept {
    Route route{};
    for (int step = 0; step < S; ++step) {
        route.actions[step] = kStay;
    }
    int position = start;
    int held = initial_held;
    int gold_delta[kCells];
    std::memset(gold_delta, 0xff, sizeof(gold_delta));
    int visited[S + 1] = {start};
    int visited_count = 1;
    const int other_row = rowOf(other);
    const int other_col = colOf(other);
    for (int step = 0; step < S; ++step) {
        int best_action = kStay;
        int best_score = kBadScore;
        const int target_distance = target >= 0 ?
            manhattan(position, target) : 0;
        int gold_cache[4];
        for (int action = 0; action < 4; ++action) {
            const int next = adjacentCell(position, action);
            gold_cache[action] = next >= 0 ?
                goldLeft(context, next, gold_delta) : 0;
        }
        for (int action = 0; action < 4; ++action) {
            const int next = adjacentCell(position, action);
            if (next < 0 || next == other ||
                testBit(context.forbidden, next)) {
                continue;
            }
            const int remaining = gold_cache[action];
            int score = pickupAmount(remaining) * 1200;
            int lookahead = 0;
            if (remaining == 0 && context.gold_count >= 2 &&
                context.gold_count <= 9 && step != 5) {
                for (int next_action = 0; next_action < 4; ++next_action) {
                    const int after = adjacentCell(next, next_action);
                    if (after < 0 || after == other ||
                        testBit(context.forbidden, after)) {
                        continue;
                    }
                    const int after_gold = after == position ? 0 :
                        estimatedGold(context, after);
                    const int next_pickup = pickupAmount(after_gold);
                    if (next_pickup > lookahead) {
                        lookahead = next_pickup;
                    }
                }
            }
            score += lookahead * 520;
            if (target >= 0) {
                score += (target_distance - manhattan(next, target)) * 280;
            }
            int separation = absInt(rowOf(next) - other_row) +
                             absInt(colOf(next) - other_col);
            score += (separation > 8 ? 8 : separation) * 3;
            for (int index = 0; index < visited_count; ++index) {
                if (visited[index] == next) {
                    score -= 45;
                    break;
                }
            }
            if (score > best_score) {
                best_score = score;
                best_action = action;
            }
        }
        route.actions[step] = static_cast<std::uint8_t>(best_action);
        if (best_action == kStay) {
            continue;
        }
        position = adjacentCell(position, best_action);
        visited[visited_count++] = position;
        held += collectGold(context, position, gold_delta);
    }
    return route;
}

GameOutput chooseFastPlan(const TurnContext& context) noexcept {
    const int starts[2] = {
        cellIndex(context.input->my_units[0].row,
                  context.input->my_units[0].col),
        cellIndex(context.input->my_units[1].row,
                  context.input->my_units[1].col)};
    int targets[2] = {-1, -1};
    for (int unit = 0; unit < 2; ++unit) {
        int best_priority = kBadScore;
        for (int index = 0; index < context.gold_count; ++index) {
            const int cell = context.gold_cells[index];
            if (testBit(context.forbidden, cell)) {
                continue;
            }
            const int gold = estimatedGold(context, cell);
            if (gold <= 0) {
                continue;
            }
            int priority = pickupAmount(gold) * 300;
            priority -= manhattan(starts[unit], cell) * 100;
            if (priority > best_priority) {
                best_priority = priority;
                targets[unit] = cell;
            }
        }
    }
    const Route routes[2] = {
        buildFastRoute(context, starts[0], starts[1], targets[0],
                       clampNonnegative(context.input->my_units_gold[0],
                                        1000000000)),
        buildFastRoute(context, starts[1], starts[0], targets[1],
                       clampNonnegative(context.input->my_units_gold[1],
                                        1000000000))};
    GameOutput best = fallbackOutput();
    std::int64_t best_score = kBadScore;
    int best_ends[2] = {starts[0], starts[1]};
    constexpr int splits[3] = {0, 3, 6};
    for (int split_index = 0; split_index < 3; ++split_index) {
        const int split = splits[split_index];
        GameOutput candidate = fallbackOutput();
        candidate.k = split;
        for (int step = 0; step < split; ++step) {
            candidate.actions[step] = routes[0].actions[step];
        }
        for (int step = 0; step < S - split; ++step) {
            candidate.actions[split + step] = routes[1].actions[step];
        }
        int ends[2];
        const std::int64_t score = evaluateJoint(context, candidate, ends) -
                                   absInt(split - 3) * 3;
        if (score > best_score) {
            best_score = score;
            best = candidate;
            best_ends[0] = ends[0];
            best_ends[1] = ends[1];
        }
    }
    g_state.last_visit_round[best_ends[0]] = context.input->round;
    g_state.last_visit_round[best_ends[1]] = context.input->round;
    return best;
}

int chooseVision(const TurnContext& context) noexcept {
    const GameInput& input = *context.input;
    if (input.round < 2 || input.round >= 490 ||
        input.round - g_state.last_purchase_round < 8 ||
        context.visible_gold_sum >= 12) {
        return 0;
    }
    const int wealth = clampNonnegative(input.my_units_gold[0], 1000000000) +
                       clampNonnegative(input.my_units_gold[1], 1000000000);
    if (wealth - g_state.vision_spend < 10) {
        return 0;
    }
    const bool behind = input.gold_opp > wealth + 25;
    int stale = 0;
    for (int unit = 0; unit < 2; ++unit) {
        const int cell = cellIndex(input.my_units[unit].row,
                                   input.my_units[unit].col);
        const int seen = g_state.last_seen_round[cell];
        stale += seen < 0 ? 16 : input.round - seen;
    }
    if (!behind && stale < 8 && context.visible_gold_sum > 0) {
        return 0;
    }
    g_state.last_purchase_round = input.round;
    g_state.vision_spend += 2;
    return 1;
}

}  // namespace

extern "C" GameOutput moveDecision(const GameInput* input) {
    if (input == nullptr || input->round < 0 || input->round > 1000000 ||
        !validPosition(input->my_units[0]) ||
        !validPosition(input->my_units[1]) ||
        (input->my_units[0].row == input->my_units[1].row &&
         input->my_units[0].col == input->my_units[1].col)) {
        return fallbackOutput();
    }
    prepareState(*input);
    TurnContext context{};
    buildContext(*input, context);
    GameOutput output = chooseFastPlan(context);
    output.vp = chooseVision(context);
    return output;
}
