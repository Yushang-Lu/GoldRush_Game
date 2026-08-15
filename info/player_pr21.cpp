// player.cpp — GoldRush 2.0 确定性有限候选联合规划器
//
// 每个角色按 0..6 步生成固定宽度、端点多样化的路径束，再对互补步数的
// 两条路径做联合复评。最终评分仍精确模拟当前可见规则；跨回合静态/动态
// 记忆、快照热度和探索评分在终态上提供确定性 tie-break。动态候选状态
// 只记录本候选实际修改过的格子，敌方先后手用有界双场景混合处理。
//
// 编译: make -> 生成 player.so
#include <cstring>
#include <limits>

#include "game_api.h"

#ifndef GOLDRUSH_FAST_PLANNER
#define GOLDRUSH_FAST_PLANNER 0
#endif

#ifndef GOLDRUSH_TURBO_PLANNER
#define GOLDRUSH_TURBO_PLANNER 1
#endif

#if GOLDRUSH_FAST_PLANNER != 0 && GOLDRUSH_FAST_PLANNER != 1
#error "GOLDRUSH_FAST_PLANNER must be exactly 0 or 1"
#endif

#if GOLDRUSH_TURBO_PLANNER != 0 && GOLDRUSH_TURBO_PLANNER != 1
#error "GOLDRUSH_TURBO_PLANNER must be exactly 0 or 1"
#endif

#if GOLDRUSH_FAST_PLANNER && GOLDRUSH_TURBO_PLANNER
#error "GOLDRUSH_FAST_PLANNER and GOLDRUSH_TURBO_PLANNER are mutually exclusive"
#endif

namespace {

constexpr int ACTION_COUNT = 5;
constexpr int PATH_BEAM_WIDTH = 32;
constexpr int PATH_EXPANSION_COUNT = PATH_BEAM_WIDTH * ACTION_COUNT;
constexpr int PATH_DIVERSE_ENDPOINTS = PATH_BEAM_WIDTH / 2;
constexpr int DR[5] = {-1, 1, 0, 0, 0};
constexpr int DC[5] = {0, 0, -1, 1, 0};
constexpr int TERRAIN_UNKNOWN = -5;
constexpr int TERRAIN_WALL = -1;
constexpr int TERRAIN_FREE = 0;
constexpr int BOMB_UNKNOWN = -1;
constexpr int BOMB_ABSENT = 0;
constexpr int BOMB_PRESENT = 1;
constexpr int VIEW_RADIUS = 2;
constexpr int ENEMY_MAX_STEPS = S;
constexpr int REGION_CENTER = 0;
constexpr int REGION_TOP = 1;
constexpr int REGION_LEFT = 2;
constexpr int REGION_BOTTOM = 3;
constexpr int REGION_RIGHT = 4;
constexpr int MAX_HEAT = 4000;
constexpr int REGION_REFERENCE_AREA = 52;
constexpr int MAX_COMPETING_OCCUPANTS = 8;
// Snapshot competition inputs are capped at 24 transitions and 8 occupants;
// their maximum raw discounts are 24*12 and 8*40 heat points respectively.
constexpr int MAX_REGION_TRANSITIONS = 24;
constexpr int REGION_HYSTERESIS_MARGIN = 160;
constexpr int VISION_LAST_ROUND = 499;
constexpr int VISION_WIDE_VIEW_ROUND = 100;
constexpr int VISIBLE_GOLD_VISION_CUTOFF = 6;
constexpr int VP1_COST = 2;
constexpr int VP2_COST = 3;
constexpr int VISION_REFRESH_INTERVAL = 6;
constexpr int VISION_MATCH_BUDGET = 250;

enum EnemyTiming {
    OUR_TURN_FIRST = 0,   // 当前敌格仍被敌方占用，进入即硬碰撞。
    OUR_TURN_SECOND = 1,  // 敌方已行动，原敌格可进入，但风险只软计分。
};

struct RegionMemory {
    int last_round;
    int enter;
    int leave;
    int generated;
    int collected;
    int remaining;
    int occupants;
    int heat;
};

struct PersistentState {
    bool initialized;
    int last_round;
    int last_snapshot_begin;
    int last_snapshot_end;
    int vision_spent;
    int vision_last_purchase_round;
    int vision_last_decision_round;
    int vision_last_decision_vp;
    int committed_region;
    Position last_units[2];
    long long last_gold[2];
    int terrain[GRID_SIZE][GRID_SIZE];
    int terrain_last_seen[GRID_SIZE][GRID_SIZE];
    int gold[GRID_SIZE][GRID_SIZE];
    int gold_last_seen[GRID_SIZE][GRID_SIZE];
    int bomb_state[GRID_SIZE][GRID_SIZE];
    int bomb_last_seen[GRID_SIZE][GRID_SIZE];
    int npc_count[GRID_SIZE][GRID_SIZE];
    int npc_last_seen[GRID_SIZE][GRID_SIZE];
    bool enemy_present[GRID_SIZE][GRID_SIZE];
    int enemy_last_seen[GRID_SIZE][GRID_SIZE];
    RegionMemory regions[REGION_COUNT];
};

PersistentState g_state{};

struct Model {
    bool valid;
    int round;
    Position starts[2];
    long long starting_gold[2];
    int terrain[GRID_SIZE][GRID_SIZE];
    int terrain_last_seen[GRID_SIZE][GRID_SIZE];
    bool current_visible[GRID_SIZE][GRID_SIZE];
    bool all_visible;
    bool no_bomb_or_trample;
    bool fast_visible_execution;
    int dynamic_tiles[GRID_SIZE][GRID_SIZE];
    int npc_count[GRID_SIZE][GRID_SIZE];
    bool enemy_block[GRID_SIZE][GRID_SIZE];
    Position enemy_positions[2];
    int enemy_count;
    bool enemy_soft_active;
    unsigned char enemy_reachable[GRID_SIZE][GRID_SIZE];
    unsigned char enemy_competition[GRID_SIZE][GRID_SIZE];
    unsigned char enemy_memory_soft[GRID_SIZE][GRID_SIZE];
    int remembered_gold[GRID_SIZE][GRID_SIZE];
    int remembered_gold_last_seen[GRID_SIZE][GRID_SIZE];
    int remembered_bomb_state[GRID_SIZE][GRID_SIZE];
    int remembered_bomb_last_seen[GRID_SIZE][GRID_SIZE];
    int remembered_npc_count[GRID_SIZE][GRID_SIZE];
    int remembered_npc_last_seen[GRID_SIZE][GRID_SIZE];
    bool remembered_enemy[GRID_SIZE][GRID_SIZE];
    int remembered_enemy_last_seen[GRID_SIZE][GRID_SIZE];
    int unknown_view_count[GRID_SIZE][GRID_SIZE];
    long long stale_gold_hint[GRID_SIZE][GRID_SIZE];
    long long hazard_hint[GRID_SIZE][GRID_SIZE];
    long long region_guidance[GRID_SIZE][GRID_SIZE];
    long long nearby_gold_value[GRID_SIZE][GRID_SIZE];
    long long nearby_gold_value_hard[GRID_SIZE][GRID_SIZE];
    Position dynamic_gold_positions[GRID_SIZE * GRID_SIZE];
    int dynamic_gold_count;
    int central_distance[GRID_SIZE][GRID_SIZE];
    unsigned char view_overlap[GRID_SIZE][GRID_SIZE][GRID_SIZE][GRID_SIZE];
    unsigned char unknown_view_overlap[GRID_SIZE][GRID_SIZE][GRID_SIZE]
                                    [GRID_SIZE];
    int miner_actor;
    int explorer_actor;
    int committed_region;
    int region_dynamic_confidence[REGION_COUNT];
    RegionMemory regions[REGION_COUNT];
};

struct Simulation {
    Position positions[2];
    long long gold[2];
    int changed_count;
    int changed_rows[S];
    int changed_cols[S];
    int changed_values[S];
    int unknown_chain[2];
};

struct SimulationStats {
    long long collected;
    long long bomb_loss;
    long long trample_loss;
    long long unknown_path_penalty;
    long long stale_hazard_penalty;
    long long enemy_path_penalty;
    long long enemy_competition_penalty;
    long long enemy_endpoint_penalty;
    long long soft_enemy_path_penalty;
    long long soft_enemy_competition_penalty;
    long long soft_enemy_endpoint_penalty;
    bool enemy_collision_attempted;
    int moved_steps;
    int actor_moved_steps[2];
    int unsafe_steps;
};

struct Candidate {
    int actions[S];
    int k;
    int order;
    long long final_gold;
    long long collected;
    long long total_loss;
    long long future_value;
    long long strategy_value;
    int moved_steps;
    long long hard_final_gold;
    long long soft_final_gold;
};

struct ScenarioEvaluation {
    Simulation simulation;
    SimulationStats stats;
    long long final_gold;
    long long total_loss;
    long long future_value;
    long long strategy_value;
};

struct PathOption {
    int actions[S];
};

struct PathSet {
    PathOption options[PATH_BEAM_WIDTH];
    int count;
};

struct PathNode {
    int actions[S];
    Simulation simulation;
    SimulationStats stats;
    long long rank;
};

bool in_bounds(const int row, const int col) {
    return row >= 0 && row < GRID_SIZE && col >= 0 && col < GRID_SIZE;
}

bool same_position(const Position& lhs, const Position& rhs) {
    return lhs.row == rhs.row && lhs.col == rhs.col;
}

bool valid_unit_layout(const GameInput* input) {
    return input != nullptr &&
           in_bounds(input->my_units[0].row, input->my_units[0].col) &&
           in_bounds(input->my_units[1].row, input->my_units[1].col) &&
           !same_position(input->my_units[0], input->my_units[1]);
}

long long ceil_fraction(const long long value, const int numerator,
                        const int denominator) {
    if (value <= 0) {
        return 0;
    }
    return (value * numerator + denominator - 1) / denominator;
}

long long pickup_amount(const int amount) {
    return ceil_fraction(static_cast<long long>(amount), 13, 20);
}

long long bomb_loss(const long long held_gold) {
    return ceil_fraction(held_gold, 1, 10);
}

long long trample_loss(const long long held_gold) {
    return ceil_fraction(held_gold, 1, 20);
}

int clamp_int(const int value, const int low, const int high) {
    return value < low ? low : (value > high ? high : value);
}

long long clamp_long_long(const long long value, const long long low,
                          const long long high) {
    return value < low ? low : (value > high ? high : value);
}

int absolute_difference(const int lhs, const int rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

int visible_gold_for_vision(const GameInput& input) {
    int visible_gold = 0;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int cell = input.grid[row][col];
            if (cell <= 0) {
                continue;
            }
            // Only the distinction below six matters.  Saturating while
            // summing also makes malformed large inputs harmless.
            const int room = VISIBLE_GOLD_VISION_CUTOFF - visible_gold;
            visible_gold += cell < room ? cell : room;
            if (visible_gold >= VISIBLE_GOLD_VISION_CUTOFF) {
                return VISIBLE_GOLD_VISION_CUTOFF;
            }
        }
    }
    return visible_gold;
}

int incremental_vision_value(const GameInput& input,
                             const PersistentState& state,
                             const int radius) {
    int value = 0;
    const int current_round = input.round < 0 ? 0 : input.round;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            // A widened view only adds cells outside the ordinary current
            // observation.  Already explored terrain still has information
            // value when its dynamic gold state was not refreshed this round.
            if (input.grid[row][col] != TERRAIN_UNKNOWN ||
                state.gold_last_seen[row][col] == current_round) {
                continue;
            }
            bool covered = false;
            for (int actor = 0; actor < 2; ++actor) {
                const Position& position = input.my_units[actor];
                if (absolute_difference(position.row, row) <= radius &&
                    absolute_difference(position.col, col) <= radius) {
                    covered = true;
                    break;
                }
            }
            if (covered) {
                // Never-seen terrain reveals both topology and current
                // dynamics; explored fog refreshes only dynamic information.
                value += state.terrain[row][col] == TERRAIN_UNKNOWN ? 2 : 1;
            }
        }
    }
    return value;
}

bool vision_purchase_affordable(const PersistentState& state,
                                const long long held_gold,
                                const int cost) {
    return held_gold >= cost && state.vision_spent <= VISION_MATCH_BUDGET - cost;
}

[[maybe_unused]] int choose_vision_purchase(const GameInput* input,
                                            const PersistentState& state) {
    if (input == nullptr || !state.initialized ||
        !in_bounds(input->my_units[0].row, input->my_units[0].col) ||
        !in_bounds(input->my_units[1].row, input->my_units[1].col) ||
        same_position(input->my_units[0], input->my_units[1])) {
        return 0;
    }

    const int round = clamp_int(input->round, 0, VISION_LAST_ROUND);
    const int remaining_rounds = VISION_LAST_ROUND - round;
    if (remaining_rounds <= 0) {
        return 0;
    }

    // A purchased widened observation arrives next round.  Its refreshed
    // terrain/dynamic information remains useful for several decisions, so
    // buying again before five intervening rounds have elapsed has poor
    // marginal value even if the ordinary 5x5 view still reports fog.
    if (state.vision_last_purchase_round >= 0 &&
        round - state.vision_last_purchase_round < VISION_REFRESH_INTERVAL) {
        return 0;
    }

    const int visible_gold = visible_gold_for_vision(*input);
    if (visible_gold >= VISIBLE_GOLD_VISION_CUTOFF) {
        return 0;
    }
    const long long held_gold =
        clamp_long_long(static_cast<long long>(input->my_units_gold[0]), 0,
                        1000000) +
        clamp_long_long(static_cast<long long>(input->my_units_gold[1]), 0,
                        1000000);
    if (!vision_purchase_affordable(state, held_gold, VP1_COST)) {
        return 0;
    }

    // Current observations have already updated persistent memory.  Count the
    // union of the two enlarged views once: unknown terrain is worth two
    // information points, while known-but-currently-fogged dynamic state is
    // worth one.  The scores are bounded by 2 * 17 * 17.
    const int vision3 = incremental_vision_value(*input, state, 3);
    const int vision4 = incremental_vision_value(*input, state, 4);
    const int outer_gain = vision4 - vision3;
    if (vision4 <= 0) {
        return 0;
    }

    // With no visible target, even a small proven information gain is worth
    // buying.  During the first fifth of the match, the lower-cost radius-3
    // view is enough to build useful memory for many future rounds.  From the
    // middle game onward, VP2 pays one extra gold only when its outer ring
    // contributes enough distinct cells to justify that marginal fee.  This
    // is a stage threshold, never a periodic purchase trigger.
    if (visible_gold == 0) {
        if (round < VISION_WIDE_VIEW_ROUND) {
            return vision3 > 0 ? 1 : 0;
        }
        if (vision_purchase_affordable(state, held_gold, VP2_COST) &&
            vision4 >= 6 && outer_gain >= 3) {
            return 2;
        }
        if (vision3 > 0) {
            return 1;
        }
        return 0;
    }

    // Visible gold competes directly with information: each known coin raises
    // the required coverage.  Near the end, fewer remaining decisions raise
    // it further.  VP2 must clear both the total radius-4 value and the value
    // of the extra ring, so it cannot win merely by containing VP1's cells.
    const int endgame_penalty =
        remaining_rounds < 6 ? 6 - remaining_rounds : 0;
    const int vp2_total_threshold =
        18 + visible_gold * 6 + endgame_penalty * 4;
    const int vp2_outer_threshold =
        8 + visible_gold * 2 + endgame_penalty * 2;
    if (vision_purchase_affordable(state, held_gold, VP2_COST) &&
        vision4 >= vp2_total_threshold &&
        outer_gain >= vp2_outer_threshold) {
        return 2;
    }

    const int vp1_threshold =
        10 + visible_gold * 4 + endgame_penalty * 3;
    return vision3 >= vp1_threshold ? 1 : 0;
}

[[maybe_unused]] int region_for_position(const Position& position) {
    if (!in_bounds(position.row, position.col)) {
        return -1;
    }
    if (position.row >= 4 && position.row <= 12 && position.col >= 4 &&
        position.col <= 12) {
        return REGION_CENTER;
    }
    if (position.row >= 0 && position.row <= 3 && position.col >= 0 &&
        position.col <= 12) {
        return REGION_TOP;
    }
    if (position.row >= 4 && position.row <= 16 && position.col >= 0 &&
        position.col <= 3) {
        return REGION_LEFT;
    }
    if (position.row >= 13 && position.row <= 16 && position.col >= 4 &&
        position.col <= 16) {
        return REGION_BOTTOM;
    }
    if (position.row >= 0 && position.row <= 12 && position.col >= 13 &&
        position.col <= 16) {
        return REGION_RIGHT;
    }
    return -1;
}

[[maybe_unused]] bool in_view_square(const Position& origin, const int row,
                                     const int col) {
    return absolute_difference(origin.row, row) <= VIEW_RADIUS &&
           absolute_difference(origin.col, col) <= VIEW_RADIUS;
}

int view_low(const int coordinate) {
    return coordinate > VIEW_RADIUS ? coordinate - VIEW_RADIUS : 0;
}

int view_high(const int coordinate) {
    const int high = coordinate + VIEW_RADIUS;
    return high < GRID_SIZE ? high : GRID_SIZE - 1;
}

int distance_to_region(const Position& position, const int region) {
    if (!in_bounds(position.row, position.col)) {
        return GRID_SIZE * 2;
    }
    int row_low = 0;
    int row_high = GRID_SIZE - 1;
    int col_low = 0;
    int col_high = GRID_SIZE - 1;
    switch (region) {
        case REGION_CENTER:
            row_low = 4;
            row_high = 12;
            col_low = 4;
            col_high = 12;
            break;
        case REGION_TOP:
            row_high = 3;
            col_high = 12;
            break;
        case REGION_LEFT:
            row_low = 4;
            col_high = 3;
            break;
        case REGION_BOTTOM:
            row_low = 13;
            col_low = 4;
            break;
        case REGION_RIGHT:
            row_high = 12;
            col_low = 13;
            break;
        default:
            return GRID_SIZE * 2;
    }
    const int row_distance =
        position.row < row_low
            ? row_low - position.row
            : (position.row > row_high ? position.row - row_high : 0);
    const int col_distance =
        position.col < col_low
            ? col_low - position.col
            : (position.col > col_high ? position.col - col_high : 0);
    return row_distance + col_distance;
}

int memory_age(const int current_round, const int last_seen) {
    if (last_seen < 0 || current_round < last_seen) {
        return GRID_SIZE * GRID_SIZE;
    }
    return current_round - last_seen;
}

int stale_confidence(const int age) {
    if (age <= 0) {
        return 100;
    }
    if (age >= 20) {
        return 4;
    }
    const int confidence = 90 - age * 4;
    return confidence < 4 ? 4 : confidence;
}

int region_snapshot_confidence(const int age) {
    // Region snapshots are global but periodic: retain 100% on arrival, lose
    // eight percentage points per round, and keep a 20% floor after ten
    // rounds.  The integer [20, 100] bound prevents an old aggregate from
    // becoming either a permanent target or an unbounded negative signal.
    if (age <= 0) {
        return 100;
    }
    return age >= 10 ? 20 : 100 - age * 8;
}

int region_area(const int region) {
    return region == REGION_CENTER ? 9 * 9 : 13 * 4;
}

int snapshot_heat(const RegionStat& stat, const int region) {
    const long long generated =
        clamp_long_long(static_cast<long long>(stat.gold_generated), 0, 500);
    const long long collected =
        clamp_long_long(static_cast<long long>(stat.gold_collected), 0, 500);
    const long long remaining =
        clamp_long_long(static_cast<long long>(stat.gold_remaining), 0, 500);
    const long long occupants =
        clamp_long_long(static_cast<long long>(stat.occupants), 0, 20);
    const long long transitions = clamp_long_long(
        static_cast<long long>(clamp_int(stat.enter, 0, 40)) +
            clamp_int(stat.leave, 0, 40),
        0, MAX_REGION_TRANSITIONS);
    // Generated and collected together measure proven turnover; remaining
    // measures the opportunity that has not yet been consumed.  Enter/leave
    // traffic and occupants are bounded competition costs, in the same raw
    // weighted-coin heat points as the positive terms.  Every term is
    // normalized to the common 52-cell peripheral area, so equal per-cell
    // statistics produce equal integer heat in every region; the returned
    // density remains inside [-4000, 4000].
    const long long throughput_and_opportunity =
        generated * 3 + collected * 2 + remaining * 5;
    const long long competition =
        transitions * 12 +
        (occupants < MAX_COMPETING_OCCUPANTS ? occupants
                                             : MAX_COMPETING_OCCUPANTS) *
            40;
    const int area = region_area(region);
    const long long value =
        (throughput_and_opportunity - competition) *
        REGION_REFERENCE_AREA / area;
    return clamp_int(static_cast<int>(value), -MAX_HEAT, MAX_HEAT);
}

void reset_persistent_state(PersistentState& state) {
    std::memset(&state, 0, sizeof(state));
    state.initialized = false;
    state.last_round = -1;
    state.last_snapshot_begin = -1;
    state.last_snapshot_end = -1;
    state.vision_last_purchase_round = -VISION_REFRESH_INTERVAL;
    state.vision_last_decision_round = -1;
    state.vision_last_decision_vp = 0;
    state.committed_region = -1;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            state.terrain[row][col] = TERRAIN_UNKNOWN;
            state.terrain_last_seen[row][col] = -1;
            state.gold_last_seen[row][col] = -1;
            state.bomb_state[row][col] = BOMB_UNKNOWN;
            state.bomb_last_seen[row][col] = -1;
            state.npc_last_seen[row][col] = -1;
            state.enemy_last_seen[row][col] = -1;
        }
    }
    for (int index = 0; index < REGION_COUNT; ++index) {
        state.regions[index].last_round = -1;
    }
}

bool looks_like_new_game(const GameInput* input,
                         const PersistentState& state) {
    if (!state.initialized || input == nullptr) {
        return false;
    }
    if (input->round == 0 || input->round < state.last_round) {
        return true;
    }
    const int displacement0 =
        absolute_difference(input->my_units[0].row, state.last_units[0].row) +
        absolute_difference(input->my_units[0].col, state.last_units[0].col);
    const int displacement1 =
        absolute_difference(input->my_units[1].row, state.last_units[1].row) +
        absolute_difference(input->my_units[1].col, state.last_units[1].col);
    if (displacement0 > S || displacement1 > S) {
        return true;
    }
    // A fresh snapshot window is a useful secondary reset signal when a host
    // reuses a round counter across matches.  It does not fire on ordinary
    // rounds because snapshot windows are monotonic within a match.
    return input->snapshot_valid != 0 && input->snapshot.window_begin == 0 &&
           state.last_snapshot_begin > 0;
}

void observe_snapshot(const GameInput& input, PersistentState& state,
                      const int round) {
    if (input.snapshot_valid == 0) {
        return;
    }
    for (int index = 0; index < REGION_COUNT; ++index) {
        int region_index = input.snapshot.regions[index].id - 1;
        if (region_index < 0 || region_index >= REGION_COUNT) {
            region_index = index;
        }
        const RegionStat& stat = input.snapshot.regions[index];
        RegionMemory& memory = state.regions[region_index];
        memory.last_round = round;
        memory.enter = clamp_int(stat.enter, 0, 40);
        memory.leave = clamp_int(stat.leave, 0, 40);
        memory.generated = clamp_int(stat.gold_generated, 0, 500);
        memory.collected = clamp_int(stat.gold_collected, 0, 500);
        memory.remaining = clamp_int(stat.gold_remaining, 0, 500);
        memory.occupants = clamp_int(stat.occupants, 0, 20);
        memory.heat = snapshot_heat(stat, region_index);
    }

    int best_region = 0;
    for (int index = 1; index < REGION_COUNT; ++index) {
        if (state.regions[index].heat > state.regions[best_region].heat) {
            best_region = index;
        }
    }
    if (state.regions[best_region].heat <= 0) {
        state.committed_region = -1;
    } else if (state.committed_region < 0 ||
               state.committed_region >= REGION_COUNT) {
        state.committed_region = best_region;
    } else if (best_region != state.committed_region &&
               (state.regions[state.committed_region].heat <= 0 ||
                state.regions[best_region].heat >
                    state.regions[state.committed_region].heat +
                        REGION_HYSTERESIS_MARGIN)) {
        // A challenger must lead by more than 160 heat points.  The margin is
        // only 4% of the [-4000, 4000] heat range and never enters an economic
        // score, but it prevents near-equal snapshots from flipping targets.
        state.committed_region = best_region;
    }
    state.last_snapshot_begin = input.snapshot.window_begin;
    state.last_snapshot_end = input.snapshot.window_end;
}

void mark_seen_free(PersistentState& state, const int row, const int col,
                    const int round) {
    if (!in_bounds(row, col)) {
        return;
    }
    if (state.terrain[row][col] != TERRAIN_WALL) {
        state.terrain[row][col] = TERRAIN_FREE;
        state.terrain_last_seen[row][col] = round;
    }
}

[[maybe_unused]] void observe_input(const GameInput* input) {
    if (input == nullptr) {
        return;
    }
    if (!g_state.initialized || looks_like_new_game(input, g_state)) {
        reset_persistent_state(g_state);
    }

    const int round = input->round < 0 ? 0 : input->round;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int cell = input->grid[row][col];
            if (cell == TERRAIN_UNKNOWN) {
                continue;
            }
            const bool was_wall = g_state.terrain[row][col] == TERRAIN_WALL;
            if (cell == TERRAIN_WALL) {
                g_state.terrain[row][col] = TERRAIN_WALL;
                g_state.terrain_last_seen[row][col] = round;
            } else if (!was_wall) {
                g_state.terrain[row][col] = TERRAIN_FREE;
                g_state.terrain_last_seen[row][col] = round;
            }
            g_state.gold[row][col] = cell > 0 ? cell : 0;
            g_state.gold_last_seen[row][col] = round;
            g_state.bomb_state[row][col] =
                cell == -3 ? BOMB_PRESENT : BOMB_ABSENT;
            g_state.bomb_last_seen[row][col] = round;
            g_state.npc_count[row][col] = 0;
            g_state.npc_last_seen[row][col] = round;
            g_state.enemy_present[row][col] = false;
            g_state.enemy_last_seen[row][col] = round;
        }
    }

    for (int index = 0; index < 2; ++index) {
        mark_seen_free(g_state, input->my_units[index].row,
                       input->my_units[index].col, round);
    }

    int npc_limit = clamp_int(input->num_visible_npcs, 0, MAX_NPCS);
    for (int index = 0; index < npc_limit; ++index) {
        const Position position = input->visible_npcs[index].pos;
        if (!in_bounds(position.row, position.col)) {
            continue;
        }
        mark_seen_free(g_state, position.row, position.col, round);
        if (g_state.npc_last_seen[position.row][position.col] != round) {
            g_state.npc_count[position.row][position.col] = 0;
            g_state.npc_last_seen[position.row][position.col] = round;
        }
        if (g_state.npc_count[position.row][position.col] < MAX_NPCS) {
            ++g_state.npc_count[position.row][position.col];
        }
    }

    for (int index = 0; index < 2; ++index) {
        const Position position = input->visible_enemies[index];
        if (!in_bounds(position.row, position.col)) {
            continue;
        }
        mark_seen_free(g_state, position.row, position.col, round);
        g_state.enemy_present[position.row][position.col] = true;
        g_state.enemy_last_seen[position.row][position.col] = round;
    }

    observe_snapshot(*input, g_state, round);
    g_state.last_round = round;
    g_state.last_units[0] = input->my_units[0];
    g_state.last_units[1] = input->my_units[1];
    g_state.last_gold[0] = input->my_units_gold[0] > 0
                                ? input->my_units_gold[0]
                                : 0;
    g_state.last_gold[1] = input->my_units_gold[1] > 0
                                ? input->my_units_gold[1]
                                : 0;
    g_state.initialized = true;
}

void prepare_model_metrics(Model& model);

Model make_model_with_memory(const GameInput* input,
                             const PersistentState* memory) {
    Model model{};
    if (!valid_unit_layout(input)) {
        return model;
    }

    model.valid = true;

    model.round = input->round < 0 ? 0 : input->round;
    model.starts[0] = input->my_units[0];
    model.starts[1] = input->my_units[1];
    model.starting_gold[0] = input->my_units_gold[0] > 0
                                 ? static_cast<long long>(input->my_units_gold[0])
                                 : 0;
    model.starting_gold[1] = input->my_units_gold[1] > 0
                                 ? static_cast<long long>(input->my_units_gold[1])
                                 : 0;
    model.dynamic_gold_count = 0;
    model.all_visible = true;

    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int cell = input->grid[row][col];
            model.current_visible[row][col] = cell != TERRAIN_UNKNOWN;
            if (!model.current_visible[row][col]) {
                model.all_visible = false;
            }
            if (memory != nullptr) {
                model.terrain[row][col] = memory->terrain[row][col];
                model.terrain_last_seen[row][col] =
                    memory->terrain_last_seen[row][col];
                model.remembered_gold[row][col] = memory->gold[row][col];
                model.remembered_gold_last_seen[row][col] =
                    memory->gold_last_seen[row][col];
                model.remembered_bomb_state[row][col] =
                    memory->bomb_state[row][col];
                model.remembered_bomb_last_seen[row][col] =
                    memory->bomb_last_seen[row][col];
                model.remembered_npc_count[row][col] =
                    memory->npc_count[row][col];
                model.remembered_npc_last_seen[row][col] =
                    memory->npc_last_seen[row][col];
                model.remembered_enemy[row][col] =
                    memory->enemy_present[row][col];
                model.remembered_enemy_last_seen[row][col] =
                    memory->enemy_last_seen[row][col];
            } else {
                model.terrain[row][col] =
                    cell == TERRAIN_UNKNOWN
                        ? TERRAIN_UNKNOWN
                        : (cell == TERRAIN_WALL ? TERRAIN_WALL
                                                 : TERRAIN_FREE);
                model.terrain_last_seen[row][col] =
                    cell == TERRAIN_UNKNOWN ? -1 : model.round;
                model.remembered_gold[row][col] = cell > 0 ? cell : 0;
                model.remembered_gold_last_seen[row][col] =
                    cell > 0 ? model.round : -1;
                model.remembered_bomb_state[row][col] =
                    cell == -3 ? BOMB_PRESENT
                                : (cell == TERRAIN_UNKNOWN ? BOMB_UNKNOWN
                                                            : BOMB_ABSENT);
                model.remembered_bomb_last_seen[row][col] =
                    cell == TERRAIN_UNKNOWN ? -1 : model.round;
                model.remembered_npc_count[row][col] = 0;
                model.remembered_npc_last_seen[row][col] = -1;
                model.remembered_enemy[row][col] = false;
                model.remembered_enemy_last_seen[row][col] = -1;
            }
            if (model.current_visible[row][col] && cell == TERRAIN_WALL) {
                model.terrain[row][col] = TERRAIN_WALL;
            }
            model.dynamic_tiles[row][col] =
                model.current_visible[row][col] &&
                        (cell == -3 || cell > 0)
                    ? cell
                    : 0;
            if (model.dynamic_tiles[row][col] > 0) {
                model.dynamic_gold_positions[model.dynamic_gold_count++] =
                    Position{row, col};
            }
            model.npc_count[row][col] = 0;
            model.enemy_block[row][col] = false;
        }
    }

    if (memory != nullptr) {
        for (int row = 0; row < GRID_SIZE; ++row) {
            for (int col = 0; col < GRID_SIZE; ++col) {
                if (memory->npc_last_seen[row][col] == model.round) {
                    model.npc_count[row][col] = memory->npc_count[row][col];
                }
                if (memory->enemy_last_seen[row][col] == model.round) {
                    model.enemy_block[row][col] =
                        memory->enemy_present[row][col];
                }
            }
        }
        std::memcpy(model.regions, memory->regions, sizeof(model.regions));
        model.committed_region = memory->committed_region;
    } else {
        model.committed_region = -1;
    }

    for (int enemy = 0; enemy < 2; ++enemy) {
        const Position position = input->visible_enemies[enemy];
        if (in_bounds(position.row, position.col)) {
            model.enemy_block[position.row][position.col] = true;
            model.current_visible[position.row][position.col] = true;
            if (model.enemy_count < 2) {
                model.enemy_positions[model.enemy_count] = position;
                ++model.enemy_count;
            }
        }
    }

    for (int index = 0; index < 2; ++index) {
        const Position position = input->my_units[index];
        if (in_bounds(position.row, position.col)) {
            model.current_visible[position.row][position.col] = true;
        }
    }

    int npc_limit = input->num_visible_npcs;
    if (npc_limit < 0) {
        npc_limit = 0;
    }
    if (npc_limit > MAX_NPCS) {
        npc_limit = MAX_NPCS;
    }
    for (int index = 0; index < npc_limit; ++index) {
        const Position position = input->visible_npcs[index].pos;
        if (in_bounds(position.row, position.col)) {
            if (memory == nullptr) {
                ++model.npc_count[position.row][position.col];
            }
            model.current_visible[position.row][position.col] = true;
        }
    }

    prepare_model_metrics(model);
    return model;
}

[[maybe_unused]] Model make_model(const GameInput* input) {
    return make_model_with_memory(input, nullptr);
}

[[maybe_unused]] int view_overlap_count(const Position& lhs,
                                        const Position& rhs) {
    const int row_low =
        view_low(lhs.row) > view_low(rhs.row) ? view_low(lhs.row)
                                              : view_low(rhs.row);
    const int row_high =
        view_high(lhs.row) < view_high(rhs.row) ? view_high(lhs.row)
                                                : view_high(rhs.row);
    const int col_low =
        view_low(lhs.col) > view_low(rhs.col) ? view_low(lhs.col)
                                              : view_low(rhs.col);
    const int col_high =
        view_high(lhs.col) < view_high(rhs.col) ? view_high(lhs.col)
                                                : view_high(rhs.col);
    if (row_low > row_high || col_low > col_high) {
        return 0;
    }
    return (row_high - row_low + 1) * (col_high - col_low + 1);
}

int prefix_rectangle_sum(const int prefix[GRID_SIZE + 1][GRID_SIZE + 1],
                         const int row_low, const int row_high,
                         const int col_low, const int col_high) {
    if (row_low > row_high || col_low > col_high) {
        return 0;
    }
    return prefix[row_high + 1][col_high + 1] -
           prefix[row_low][col_high + 1] - prefix[row_high + 1][col_low] +
           prefix[row_low][col_low];
}

struct GeometryCache {
    bool initialized;
    unsigned char view_overlap[GRID_SIZE][GRID_SIZE][GRID_SIZE][GRID_SIZE];
    int central_distance[GRID_SIZE][GRID_SIZE];
};

GeometryCache g_geometry_cache{};

void ensure_geometry_cache() {
    if (g_geometry_cache.initialized) {
        return;
    }
    for (int row0 = 0; row0 < GRID_SIZE; ++row0) {
        for (int col0 = 0; col0 < GRID_SIZE; ++col0) {
            const Position lhs{row0, col0};
            for (int row1 = 0; row1 < GRID_SIZE; ++row1) {
                for (int col1 = 0; col1 < GRID_SIZE; ++col1) {
                    const Position rhs{row1, col1};
                    g_geometry_cache.view_overlap[row0][col0][row1][col1] =
                        static_cast<unsigned char>(
                            view_overlap_count(lhs, rhs));
                }
            }
            g_geometry_cache.central_distance[row0][col0] =
                distance_to_region(lhs, REGION_CENTER);
        }
    }
    g_geometry_cache.initialized = true;
}

int compute_region_dynamic_confidence(const Model& model, const int region) {
    long long freshness_sum = 0;
    int cells = 0;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            if (region_for_position(Position{row, col}) != region ||
                model.terrain[row][col] == TERRAIN_WALL) {
                continue;
            }
            int last_dynamic_seen =
                model.remembered_gold_last_seen[row][col];
            if (model.remembered_bomb_last_seen[row][col] >
                last_dynamic_seen) {
                last_dynamic_seen =
                    model.remembered_bomb_last_seen[row][col];
            }
            if (model.remembered_npc_last_seen[row][col] >
                last_dynamic_seen) {
                last_dynamic_seen = model.remembered_npc_last_seen[row][col];
            }
            if (model.remembered_enemy_last_seen[row][col] >
                last_dynamic_seen) {
                last_dynamic_seen =
                    model.remembered_enemy_last_seen[row][col];
            }
            const int age = memory_age(model.round, last_dynamic_seen);
            freshness_sum += model.current_visible[row][col]
                                 ? 100
                                 : stale_confidence(age);
            ++cells;
        }
    }
    if (cells == 0) {
        return 50;
    }
    const int mean_freshness =
        clamp_int(static_cast<int>(freshness_sum / cells), 0, 100);
    // Snapshot `remaining` is itself current global evidence, so local dynamic
    // fog may only halve its confidence.  Mapping mean freshness into the
    // integer [50, 100] interval keeps this modifier bounded and symmetric.
    return 50 + mean_freshness / 2;
}

long long compute_region_guidance(const Model& model,
                                  const Position& position) {
    long long value = 0;
    for (int index = 0; index < REGION_COUNT; ++index) {
        const RegionMemory& region = model.regions[index];
        if (region.last_round < 0) {
            continue;
        }
        const int age = memory_age(model.round, region.last_round);
        const int snapshot_confidence = region_snapshot_confidence(age);
        const int dynamic_confidence =
            model.region_dynamic_confidence[index];
        long long committed_heat = region.heat;
        if (model.committed_region >= 0 &&
            index != model.committed_region && committed_heat > 0) {
            // While a target is retained, competing positive signals lose at
            // most the same 160-point margin.  Subtraction (instead of boosting
            // the target) preserves the documented [-4000, 4000] heat bound.
            committed_heat =
                committed_heat > REGION_HYSTERESIS_MARGIN
                    ? committed_heat - REGION_HYSTERESIS_MARGIN
                    : 0;
        }
        const long long heat = committed_heat * snapshot_confidence *
                               dynamic_confidence / 10000;
        const int distance = distance_to_region(position, index);
        const int closeness = 20 - (distance > 20 ? 20 : distance);
        if (closeness > 0) {
            value += heat * closeness / 20;
        }
    }
    return clamp_long_long(value, -MAX_HEAT, MAX_HEAT);
}

long long region_guidance(const Model& model, const Position& position) {
    return model.region_guidance[position.row][position.col];
}

long long region_progress_value(const Model& model, const Position& origin,
                                const Position& endpoint) {
    // Beam nodes at every depth receive only their bounded change in regional
    // guidance from the actor's start.  Thus intermediate progress can retain
    // a useful route without repeatedly accumulating heat; endpoint scoring
    // below remains the final joint-candidate signal.
    return clamp_long_long(region_guidance(model, endpoint) -
                               region_guidance(model, origin),
                           -MAX_HEAT, MAX_HEAT);
}

long long visible_gold_near(const Model& model, const Position& position) {
    return model.nearby_gold_value_hard[position.row][position.col];
}

long long miner_preference(const Model& model, const int actor) {
    const Position& position = model.starts[actor];
    const int central_distance = model.central_distance[position.row][position.col];
    long long value = static_cast<long long>(20 -
                                             (central_distance > 20
                                                  ? 20
                                                  : central_distance)) *
                      8;
    value += visible_gold_near(model, position) * 2;
    value -= clamp_long_long(model.starting_gold[actor], 0, 400);
    value -= model.hazard_hint[position.row][position.col] / 2;
    if (model.npc_count[position.row][position.col] >= 3) {
        value -= 40;
    }
    if (model.enemy_block[position.row][position.col]) {
        value -= 40;
    }
    return value;
}

void prepare_model_metrics(Model& model) {
    ensure_geometry_cache();
    model.no_bomb_or_trample = model.all_visible;
    model.fast_visible_execution = model.all_visible;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            if (model.terrain[row][col] == TERRAIN_UNKNOWN ||
                model.npc_count[row][col] >= 3) {
                model.fast_visible_execution = false;
            }
            if (model.terrain[row][col] == TERRAIN_UNKNOWN ||
                model.dynamic_tiles[row][col] == -3 ||
                model.npc_count[row][col] >= 3) {
                model.no_bomb_or_trample = false;
            }
        }
    }
    for (int region = 0; region < REGION_COUNT; ++region) {
        model.region_dynamic_confidence[region] =
            compute_region_dynamic_confidence(model, region);
    }
    int unknown_prefix[GRID_SIZE + 1][GRID_SIZE + 1]{};
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            unknown_prefix[row + 1][col + 1] =
                unknown_prefix[row][col + 1] + unknown_prefix[row + 1][col] -
                unknown_prefix[row][col] +
                (model.terrain[row][col] == TERRAIN_UNKNOWN ? 1 : 0);
        }
    }

    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const Position endpoint{row, col};
            model.unknown_view_count[row][col] = prefix_rectangle_sum(
                unknown_prefix, view_low(row), view_high(row), view_low(col),
                view_high(col));

            long long stale_gold = 0;
            long long hazard = 0;
            const int hint_row_low = row - 4 > 0 ? row - 4 : 0;
            const int hint_row_high =
                row + 4 < GRID_SIZE ? row + 4 : GRID_SIZE - 1;
            const int hint_col_low = col - 4 > 0 ? col - 4 : 0;
            const int hint_col_high =
                col + 4 < GRID_SIZE ? col + 4 : GRID_SIZE - 1;
            if (!model.all_visible) {
                for (int hint_row = hint_row_low; hint_row <= hint_row_high;
                     ++hint_row) {
                    for (int hint_col = hint_col_low;
                         hint_col <= hint_col_high; ++hint_col) {
                    const int distance =
                        absolute_difference(row, hint_row) +
                        absolute_difference(col, hint_col);
                    if (distance > 4 ||
                        model.terrain[hint_row][hint_col] == TERRAIN_WALL) {
                        continue;
                    }
                    const int age = memory_age(
                        model.round,
                        model.remembered_gold_last_seen[hint_row][hint_col]);
                    if (!model.current_visible[hint_row][hint_col] &&
                        model.remembered_gold[hint_row][hint_col] > 0) {
                        const int confidence = stale_confidence(age);
                        stale_gold += static_cast<long long>(
                                           model.remembered_gold[hint_row][hint_col]) *
                                       confidence / (100 * (distance + 1));
                    }

                    const int bomb_age = memory_age(
                        model.round,
                        model.remembered_bomb_last_seen[hint_row][hint_col]);
                    if (!model.current_visible[hint_row][hint_col] &&
                        model.remembered_bomb_state[hint_row][hint_col] ==
                            BOMB_PRESENT) {
                        hazard += 18LL * stale_confidence(bomb_age) /
                                  (100 * (distance + 1));
                    }
                    const int npc_age = memory_age(
                        model.round,
                        model.remembered_npc_last_seen[hint_row][hint_col]);
                    if (!model.current_visible[hint_row][hint_col] &&
                        model.remembered_npc_count[hint_row][hint_col] >= 3) {
                        hazard += static_cast<long long>(
                                      model.remembered_npc_count[hint_row][hint_col] -
                                      2) *
                                  10 * stale_confidence(npc_age) /
                                  (100 * (distance + 1));
                    }
                    const int enemy_age = memory_age(
                        model.round,
                        model.remembered_enemy_last_seen[hint_row][hint_col]);
                    if (!model.current_visible[hint_row][hint_col] &&
                        model.remembered_enemy[hint_row][hint_col]) {
                        hazard += 12LL * stale_confidence(enemy_age) /
                                  (100 * (distance + 1));
                    }
                    }
                }
            }
            model.stale_gold_hint[row][col] =
                clamp_long_long(stale_gold, 0, 1000);
            model.hazard_hint[row][col] = clamp_long_long(hazard, 0, 1000);
            model.region_guidance[row][col] =
                compute_region_guidance(model, endpoint);

            long long nearby_gold = 0;
            long long nearby_gold_hard = 0;
            const int row_low = row - 3 > 0 ? row - 3 : 0;
            const int row_high = row + 3 < GRID_SIZE ? row + 3 : GRID_SIZE - 1;
            const int col_low = col - 3 > 0 ? col - 3 : 0;
            const int col_high = col + 3 < GRID_SIZE ? col + 3 : GRID_SIZE - 1;
            if (model.dynamic_gold_count > 0) {
                for (int gold_row = row_low; gold_row <= row_high;
                     ++gold_row) {
                    for (int gold_col = col_low; gold_col <= col_high;
                         ++gold_col) {
                    const int distance =
                        absolute_difference(row, gold_row) +
                        absolute_difference(col, gold_col);
                    const int amount = model.dynamic_tiles[gold_row][gold_col];
                    if (distance > 3 || amount <= 0 ||
                        model.terrain[gold_row][gold_col] == TERRAIN_WALL) {
                        continue;
                    }
                    const long long contribution =
                        static_cast<long long>(amount) * 10 / (distance + 1);
                    nearby_gold += contribution;
                    if (!model.enemy_block[gold_row][gold_col]) {
                        nearby_gold_hard += contribution;
                    }
                    }
                }
            }
            model.nearby_gold_value[row][col] = nearby_gold;
            model.nearby_gold_value_hard[row][col] = nearby_gold_hard;

            int reachable = 0;
            for (int enemy = 0; enemy < model.enemy_count; ++enemy) {
                const Position enemy_position = model.enemy_positions[enemy];
                const int distance =
                    absolute_difference(row, enemy_position.row) +
                    absolute_difference(col, enemy_position.col);
                if (distance <= ENEMY_MAX_STEPS) {
                    ++reachable;
                }
            }
            model.enemy_reachable[row][col] =
                static_cast<unsigned char>(reachable);
            int competition = reachable * 22;
            if (model.enemy_block[row][col]) {
                competition += 30;
            }
            model.enemy_competition[row][col] =
                static_cast<unsigned char>(clamp_int(competition, 0, 90));

            if (model.remembered_enemy[row][col] &&
                !model.enemy_block[row][col]) {
                const int age = memory_age(
                    model.round, model.remembered_enemy_last_seen[row][col]);
                model.enemy_memory_soft[row][col] =
                    static_cast<unsigned char>(
                        24 * stale_confidence(age) / 100);
                model.enemy_soft_active = true;
            }
        }
    }

    std::memcpy(model.central_distance, g_geometry_cache.central_distance,
                sizeof(model.central_distance));
    std::memcpy(model.view_overlap, g_geometry_cache.view_overlap,
                sizeof(model.view_overlap));
    if (unknown_prefix[GRID_SIZE][GRID_SIZE] == 0) {
        std::memset(model.unknown_view_overlap, 0,
                    sizeof(model.unknown_view_overlap));
    } else {
        for (int row0 = 0; row0 < GRID_SIZE; ++row0) {
            for (int col0 = 0; col0 < GRID_SIZE; ++col0) {
                for (int row1 = 0; row1 < GRID_SIZE; ++row1) {
                    for (int col1 = 0; col1 < GRID_SIZE; ++col1) {
                        const int row_low =
                            view_low(row0) > view_low(row1) ? view_low(row0)
                            : view_low(row1);
                        const int row_high =
                            view_high(row0) < view_high(row1) ? view_high(row0)
                                                               : view_high(row1);
                        const int col_low =
                            view_low(col0) > view_low(col1) ? view_low(col0)
                            : view_low(col1);
                        const int col_high =
                            view_high(col0) < view_high(col1) ? view_high(col0)
                                                               : view_high(col1);
                        model.unknown_view_overlap[row0][col0][row1][col1] =
                            static_cast<unsigned char>(prefix_rectangle_sum(
                                unknown_prefix, row_low, row_high, col_low,
                                col_high));
                    }
                }
            }
        }
    }

    const long long preference0 = miner_preference(model, 0);
    const long long preference1 = miner_preference(model, 1);
    model.miner_actor = preference0 >= preference1 ? 0 : 1;
    model.explorer_actor = 1 - model.miner_actor;
    model.fast_visible_execution = model.fast_visible_execution &&
                                   model.enemy_count == 0 &&
                                   !model.enemy_soft_active;
}

Simulation make_simulation(const Model& model) {
    Simulation simulation{};
    simulation.positions[0] = model.starts[0];
    simulation.positions[1] = model.starts[1];
    simulation.gold[0] = model.starting_gold[0];
    simulation.gold[1] = model.starting_gold[1];
    return simulation;
}

int dynamic_change_index(const Simulation& simulation, const int row,
                         const int col) {
    for (int index = 0; index < simulation.changed_count; ++index) {
        if (simulation.changed_rows[index] == row &&
            simulation.changed_cols[index] == col) {
            return index;
        }
    }
    return -1;
}

int dynamic_tile_at(const Model& model, const Simulation& simulation,
                    const int row, const int col) {
    const int change_index = dynamic_change_index(simulation, row, col);
    return change_index >= 0 ? simulation.changed_values[change_index]
                             : model.dynamic_tiles[row][col];
}

void set_dynamic_tile(const Model& model, Simulation& simulation,
                      const int row, const int col, const int value) {
    const int change_index = dynamic_change_index(simulation, row, col);
    if (change_index >= 0) {
        simulation.changed_values[change_index] = value;
        return;
    }
    if (value == model.dynamic_tiles[row][col]) {
        return;
    }
    if (simulation.changed_count < S) {
        const int index = simulation.changed_count;
        simulation.changed_rows[index] = row;
        simulation.changed_cols[index] = col;
        simulation.changed_values[index] = value;
        ++simulation.changed_count;
    }
}

int enemy_soft_risk_at(const Model& model, const Position& position,
                       const EnemyTiming timing) {
    int risk = model.enemy_memory_soft[position.row][position.col];
    if (timing == OUR_TURN_SECOND) {
        risk += model.enemy_competition[position.row][position.col];
    }
    return clamp_int(risk, 0, 100);
}

bool blocked_target(const Model& model, const Simulation& simulation,
                    const int actor, const Position& target,
                    const EnemyTiming timing = OUR_TURN_FIRST) {
    if (!in_bounds(target.row, target.col)) {
        return true;
    }
    if (model.terrain[target.row][target.col] == TERRAIN_WALL) {
        return true;
    }
    if (timing == OUR_TURN_FIRST &&
        model.enemy_block[target.row][target.col]) {
        return true;
    }
    return same_position(target, simulation.positions[1 - actor]);
}

void execute_step(const Model& model, Simulation& simulation, const int actor,
                  const int action, const int sequence_index,
                  SimulationStats& stats,
                  const EnemyTiming timing = OUR_TURN_FIRST) {
    if (action == 4) {
        return;
    }

    if (model.fast_visible_execution) {
        const Position current = simulation.positions[actor];
        const Position target{current.row + DR[action],
                              current.col + DC[action]};
        if (blocked_target(model, simulation, actor, target, timing)) {
            return;
        }
        simulation.positions[actor] = target;
        ++stats.moved_steps;
        ++stats.actor_moved_steps[actor];

        const int tile = dynamic_tile_at(model, simulation, target.row,
                                         target.col);
        if (tile > 0) {
            const long long pickup = pickup_amount(tile);
            simulation.gold[actor] += pickup;
            stats.collected += pickup;
            set_dynamic_tile(model, simulation, target.row, target.col,
                             tile - static_cast<int>(pickup));
        } else if (tile == -3) {
            ++stats.unsafe_steps;
            const long long loss = bomb_loss(simulation.gold[actor]);
            simulation.gold[actor] -= loss;
            stats.bomb_loss += loss;
            set_dynamic_tile(model, simulation, target.row, target.col, 0);
        }
        return;
    }

    if (model.no_bomb_or_trample) {
        const Position current = simulation.positions[actor];
        const Position target{current.row + DR[action],
                              current.col + DC[action]};
        if (blocked_target(model, simulation, actor, target, timing)) {
            if (timing == OUR_TURN_FIRST && model.enemy_count > 0 &&
                in_bounds(target.row, target.col) &&
                model.terrain[target.row][target.col] != TERRAIN_WALL &&
                model.enemy_block[target.row][target.col]) {
                stats.enemy_collision_attempted = true;
            }
            return;
        }

        simulation.positions[actor] = target;
        ++stats.moved_steps;
        ++stats.actor_moved_steps[actor];

        int enemy_risk = 0;
        if (model.enemy_count > 0 || model.enemy_soft_active) {
            enemy_risk = enemy_soft_risk_at(model, target, timing);
            if (enemy_risk > 0) {
                stats.enemy_path_penalty += enemy_risk / 4 + 1;
            }
        }
        int soft_risk = 0;
        if (timing == OUR_TURN_FIRST && model.enemy_count > 0) {
            soft_risk = enemy_soft_risk_at(model, target, OUR_TURN_SECOND);
            if (soft_risk > 0) {
                stats.soft_enemy_path_penalty += soft_risk / 4 + 1;
            }
        }

        const int tile = dynamic_tile_at(model, simulation, target.row,
                                         target.col);
        if (tile > 0) {
            const long long pickup = pickup_amount(tile);
            simulation.gold[actor] += pickup;
            stats.collected += pickup;
            if (enemy_risk > 0) {
                stats.enemy_competition_penalty +=
                    pickup * enemy_risk / 100;
            }
            if (soft_risk > 0) {
                stats.soft_enemy_competition_penalty +=
                    pickup * soft_risk / 100;
            }
            set_dynamic_tile(model, simulation, target.row, target.col,
                             tile - static_cast<int>(pickup));
        }
        return;
    }

    const Position current = simulation.positions[actor];
    const Position target{current.row + DR[action], current.col + DC[action]};
    if (blocked_target(model, simulation, actor, target, timing)) {
        if (timing == OUR_TURN_FIRST && model.enemy_count > 0 &&
            in_bounds(target.row, target.col) &&
            model.terrain[target.row][target.col] != TERRAIN_WALL &&
            model.enemy_block[target.row][target.col]) {
            stats.enemy_collision_attempted = true;
        }
        return;
    }

    simulation.positions[actor] = target;
    ++stats.moved_steps;
    ++stats.actor_moved_steps[actor];

    if (model.terrain[target.row][target.col] == TERRAIN_UNKNOWN) {
        ++simulation.unknown_chain[actor];
        const int early_weight = S - sequence_index;
        stats.unknown_path_penalty +=
            static_cast<long long>(early_weight) *
            simulation.unknown_chain[actor] * 4;
    } else {
        simulation.unknown_chain[actor] = 0;
    }

    int enemy_risk = 0;
    if (model.enemy_count > 0 || model.enemy_soft_active) {
        enemy_risk = enemy_soft_risk_at(model, target, timing);
        if (enemy_risk > 0) {
            stats.enemy_path_penalty += enemy_risk / 4 + 1;
        }
    }
    if (timing == OUR_TURN_FIRST && model.enemy_count > 0) {
        const int soft_risk =
            enemy_soft_risk_at(model, target, OUR_TURN_SECOND);
        if (soft_risk > 0) {
            stats.soft_enemy_path_penalty += soft_risk / 4 + 1;
        }
    }

    if (!model.current_visible[target.row][target.col]) {
        const int bomb_age = memory_age(
            model.round,
            model.remembered_bomb_last_seen[target.row][target.col]);
        if (model.remembered_bomb_state[target.row][target.col] ==
            BOMB_PRESENT) {
            stats.stale_hazard_penalty +=
                16LL * stale_confidence(bomb_age) / 100;
        }
        const int npc_age = memory_age(
            model.round,
            model.remembered_npc_last_seen[target.row][target.col]);
        if (model.remembered_npc_count[target.row][target.col] >= 3) {
            stats.stale_hazard_penalty += static_cast<long long>(
                                              model.remembered_npc_count
                                                  [target.row][target.col] -
                                              2) *
                                          stale_confidence(npc_age);
        }
        const int enemy_age = memory_age(
            model.round,
            model.remembered_enemy_last_seen[target.row][target.col]);
        if (model.remembered_enemy[target.row][target.col]) {
            stats.stale_hazard_penalty += stale_confidence(enemy_age) / 2;
        }
    }

    const int tile = dynamic_tile_at(model, simulation, target.row, target.col);
    if (tile > 0) {
        const long long pickup = pickup_amount(tile);
        simulation.gold[actor] += pickup;
        stats.collected += pickup;
        if (enemy_risk > 0) {
            stats.enemy_competition_penalty +=
                pickup * enemy_risk / 100;
        }
        if (timing == OUR_TURN_FIRST && model.enemy_count > 0) {
            const int soft_risk =
                enemy_soft_risk_at(model, target, OUR_TURN_SECOND);
            if (soft_risk > 0) {
                stats.soft_enemy_competition_penalty +=
                    pickup * soft_risk / 100;
            }
        }
        set_dynamic_tile(model, simulation, target.row, target.col,
                         tile - static_cast<int>(pickup));
    } else if (tile == -3) {
        ++stats.unsafe_steps;
        const long long loss = bomb_loss(simulation.gold[actor]);
        simulation.gold[actor] -= loss;
        stats.bomb_loss += loss;
        set_dynamic_tile(model, simulation, target.row, target.col, 0);
    }

    if (model.npc_count[target.row][target.col] >= 3) {
        ++stats.unsafe_steps;
        const long long loss = trample_loss(simulation.gold[actor]);
        simulation.gold[actor] -= loss;
        stats.trample_loss += loss;
    }
}

void execute_actor(const Model& model, Simulation& simulation, const int actor,
                   const int actions[S], const int k,
                   SimulationStats& stats,
                   const EnemyTiming timing = OUR_TURN_FIRST) {
    const int begin = actor == 0 ? 0 : k;
    const int end = actor == 0 ? k : S;
    for (int index = begin; index < end; ++index) {
        const int local_index = index - begin;
        execute_step(model, simulation, actor, actions[index], local_index,
                     stats, timing);
    }
}

SimulationStats simulate(const Model& model, const int actions[S], const int k,
                         const int order, Simulation& simulation,
                         const EnemyTiming timing = OUR_TURN_FIRST) {
    SimulationStats stats{};
    const int first = order;
    const int second = 1 - order;
    execute_actor(model, simulation, first, actions, k, stats, timing);
    execute_actor(model, simulation, second, actions, k, stats, timing);
    if (model.enemy_count > 0 || model.enemy_soft_active) {
        stats.enemy_endpoint_penalty =
            enemy_soft_risk_at(model, simulation.positions[0], timing) +
            enemy_soft_risk_at(model, simulation.positions[1], timing);
    }
    if (timing == OUR_TURN_FIRST && model.enemy_count > 0) {
        stats.soft_enemy_endpoint_penalty =
            enemy_soft_risk_at(model, simulation.positions[0], OUR_TURN_SECOND) +
            enemy_soft_risk_at(model, simulation.positions[1], OUR_TURN_SECOND);
    }
    return stats;
}

long long remaining_gold_value(const Model& model, const Simulation& simulation,
                               const int actor,
                               const EnemyTiming timing = OUR_TURN_FIRST) {
    const Position origin = simulation.positions[actor];
    long long value =
        timing == OUR_TURN_FIRST
            ? model.nearby_gold_value_hard[origin.row][origin.col]
            : model.nearby_gold_value[origin.row][origin.col];
    for (int index = 0; index < simulation.changed_count; ++index) {
        const int row = simulation.changed_rows[index];
        const int col = simulation.changed_cols[index];
        const int distance = absolute_difference(origin.row, row) +
                             absolute_difference(origin.col, col);
        if (distance > 3 || model.terrain[row][col] == TERRAIN_WALL ||
            (timing == OUR_TURN_FIRST && model.enemy_block[row][col])) {
            continue;
        }
        const int before = model.dynamic_tiles[row][col];
        const int after = simulation.changed_values[index];
        value += static_cast<long long>(after > 0 ? after : 0) * 10 /
                     (distance + 1) -
                 static_cast<long long>(before > 0 ? before : 0) * 10 /
                     (distance + 1);
    }
    return value;
}

long long candidate_future_value(const Model& model,
                                 const Simulation& simulation,
                                 const EnemyTiming timing = OUR_TURN_FIRST) {
    return remaining_gold_value(model, simulation, 0, timing) +
           remaining_gold_value(model, simulation, 1, timing);
}

int exploration_weight(const int round) {
    if (round >= 430) {
        return 2;
    }
    if (round < 30) {
        return 28;
    }
    return 12;
}

int unknown_view_overlap(const Model& model, const Position& lhs,
                         const Position& rhs) {
    return model.unknown_view_overlap[lhs.row][lhs.col][rhs.row][rhs.col];
}

long long miner_endpoint_value(const Model& model, const Position& position) {
    const int central_distance = model.central_distance[position.row][position.col];
    const int central_closeness =
        20 - (central_distance > 20 ? 20 : central_distance);
    return static_cast<long long>(central_closeness) * 12 +
           model.stale_gold_hint[position.row][position.col] * 5 -
           model.hazard_hint[position.row][position.col] * 3 +
           region_guidance(model, position) / 2;
}

long long explorer_endpoint_value(const Model& model,
                                  const Position& position) {
    const int weight = exploration_weight(model.round);
    return static_cast<long long>(model.unknown_view_count[position.row]
                                      [position.col]) *
               weight +
           model.stale_gold_hint[position.row][position.col] * 3 +
           region_guidance(model, position) -
           model.hazard_hint[position.row][position.col] * 3;
}

long long shared_safe_activity_bonus(const SimulationStats& stats) {
    const bool safely_active =
        stats.actor_moved_steps[0] > 0 && stats.actor_moved_steps[1] > 0 &&
        stats.unsafe_steps == 0 && stats.stale_hazard_penalty == 0 &&
        stats.enemy_path_penalty == 0 &&
        stats.enemy_endpoint_penalty == 0 &&
        !stats.enemy_collision_attempted;
    if (!safely_active) {
        return 0;
    }
    const int shared_steps =
        stats.actor_moved_steps[0] < stats.actor_moved_steps[1]
            ? stats.actor_moved_steps[0]
            : stats.actor_moved_steps[1];
    return 96 + static_cast<long long>(shared_steps) * 48;
}

long long candidate_strategy_value(const Model& model,
                                   const Simulation& simulation,
                                   const SimulationStats& stats,
                                   const EnemyTiming timing = OUR_TURN_FIRST) {
    const Position& miner_position =
        simulation.positions[model.miner_actor];
    const Position& explorer_position =
        simulation.positions[model.explorer_actor];
    const int weight = exploration_weight(model.round);
    long long value = miner_endpoint_value(model, miner_position) +
                      explorer_endpoint_value(model, explorer_position);
    const int overlap =
        model.view_overlap[simulation.positions[0].row]
                          [simulation.positions[0].col]
                          [simulation.positions[1].row]
                          [simulation.positions[1].col];
    const int unknown_overlap =
        unknown_view_overlap(model, simulation.positions[0],
                             simulation.positions[1]);
    const int first_unknown =
        model.unknown_view_count[simulation.positions[0].row]
                                [simulation.positions[0].col];
    const int second_unknown =
        model.unknown_view_count[simulation.positions[1].row]
                                [simulation.positions[1].col];
    // Preserve a minimum amount of view progress for both roles when the map
    // still has safe fog.  Net gold and losses are compared before this score,
    // so a single reachable high-value target may still justify k=0/6.
    const int shared_exploration =
        first_unknown < second_unknown ? first_unknown : second_unknown;
    value += static_cast<long long>(shared_exploration) * (weight + 2);
    value -= static_cast<long long>(overlap) * (weight / 2 + 1);
    value -= static_cast<long long>(unknown_overlap) * (weight + 2);
    value -= stats.unknown_path_penalty * 3;
    value -= stats.stale_hazard_penalty * 2;
    value -= stats.enemy_path_penalty * 2;
    value -= stats.enemy_endpoint_penalty * 2;

    // Reward shared activity only inside the strategic score, after net gold,
    // realized losses, and future gold have already been evaluated.  The
    // reward is deliberately unavailable when either route encountered a
    // known/stale hazard or enemy risk, so it cannot manufacture a reason to
    // spend actions on an unsafe teammate route.  Taking the minimum makes
    // the signal actor-symmetric and leaves k=0/6 legal whenever one actor's
    // six-step route wins on an economic score.
    value += shared_safe_activity_bonus(stats);
    if (timing == OUR_TURN_SECOND && model.enemy_count > 0) {
        value -= static_cast<long long>(
                     enemy_soft_risk_at(model, miner_position, timing) +
                     enemy_soft_risk_at(model, explorer_position, timing)) *
                 2;
    }
    return value;
}

ScenarioEvaluation evaluate_scenario(const Model& model, const int actions[S],
                                     const int k, const int order,
                                     const EnemyTiming timing) {
    ScenarioEvaluation evaluation{};
    evaluation.simulation = make_simulation(model);
    evaluation.stats =
        simulate(model, actions, k, order, evaluation.simulation, timing);
    evaluation.final_gold = evaluation.simulation.gold[0] +
                            evaluation.simulation.gold[1] -
                            evaluation.stats.enemy_competition_penalty;
    evaluation.total_loss = evaluation.stats.bomb_loss +
                            evaluation.stats.trample_loss +
                            evaluation.stats.enemy_competition_penalty;
    evaluation.future_value =
        candidate_future_value(model, evaluation.simulation, timing);
    evaluation.strategy_value = candidate_strategy_value(
        model, evaluation.simulation, evaluation.stats, timing);
    return evaluation;
}

ScenarioEvaluation derive_soft_scenario(const Model& model,
                                        const ScenarioEvaluation& hard) {
    ScenarioEvaluation soft = hard;
    soft.stats.enemy_path_penalty = hard.stats.soft_enemy_path_penalty;
    soft.stats.enemy_competition_penalty =
        hard.stats.soft_enemy_competition_penalty;
    soft.stats.enemy_endpoint_penalty =
        hard.stats.soft_enemy_endpoint_penalty;
    soft.stats.enemy_collision_attempted = false;
    soft.final_gold = hard.simulation.gold[0] + hard.simulation.gold[1] -
                      soft.stats.enemy_competition_penalty;
    soft.total_loss = hard.stats.bomb_loss + hard.stats.trample_loss +
                      soft.stats.enemy_competition_penalty;
    soft.future_value =
        candidate_future_value(model, hard.simulation, OUR_TURN_SECOND);
    soft.strategy_value = candidate_strategy_value(
        model, hard.simulation, soft.stats, OUR_TURN_SECOND);
    return soft;
}

long long bounded_enemy_mix(const long long first, const long long second) {
    const long long low = first < second ? first : second;
    const long long high = first > second ? first : second;
    // Keep a conservative edge for the unknown timing while retaining a
    // bounded amount of value from the scenario in which the enemy moved.
    return low + (high - low) * 3 / 8;
}

bool lexicographically_less(const int lhs[S], const int rhs[S]) {
    for (int index = 0; index < S; ++index) {
        if (lhs[index] != rhs[index]) {
            return lhs[index] < rhs[index];
        }
    }
    return false;
}

[[maybe_unused]] bool better_candidate(const Candidate& candidate,
                                       const Candidate& best) {
    if (candidate.final_gold != best.final_gold) {
        return candidate.final_gold > best.final_gold;
    }
    if (candidate.total_loss != best.total_loss) {
        return candidate.total_loss < best.total_loss;
    }
    if (candidate.future_value != best.future_value) {
        return candidate.future_value > best.future_value;
    }
    if (candidate.strategy_value != best.strategy_value) {
        return candidate.strategy_value > best.strategy_value;
    }
    if (candidate.moved_steps != best.moved_steps) {
        // Only after every economic and strategic score is identical, prefer
        // actions that actually moved.  This avoids lexicographic stay-heavy
        // plans without forcing movement through a hazard.
        return candidate.moved_steps > best.moved_steps;
    }
    if (candidate.k != best.k) {
        const int candidate_distance = candidate.k > 3 ? candidate.k - 3
                                                         : 3 - candidate.k;
        const int best_distance = best.k > 3 ? best.k - 3 : 3 - best.k;
        if (candidate_distance != best_distance) {
            return candidate_distance < best_distance;
        }
    }
    if (candidate.order != best.order) {
        return candidate.order < best.order;
    }
    return lexicographically_less(candidate.actions, best.actions);
}

[[maybe_unused]] Candidate evaluate_candidate(const Model& model,
                                              const int actions[S],
                                              const int k, const int order) {
    const ScenarioEvaluation hard =
        evaluate_scenario(model, actions, k, order, OUR_TURN_FIRST);
    const ScenarioEvaluation soft =
        model.enemy_count == 0
            ? hard
            : (hard.stats.enemy_collision_attempted
                   ? evaluate_scenario(model, actions, k, order,
                                       OUR_TURN_SECOND)
                   : derive_soft_scenario(model, hard));

    Candidate candidate{};
    std::memcpy(candidate.actions, actions, sizeof(candidate.actions));
    candidate.k = k;
    candidate.order = order;
    candidate.final_gold =
        bounded_enemy_mix(hard.final_gold, soft.final_gold);
    candidate.collected = bounded_enemy_mix(hard.stats.collected,
                                            soft.stats.collected);
    candidate.total_loss =
        bounded_enemy_mix(hard.total_loss, soft.total_loss);
    candidate.future_value =
        bounded_enemy_mix(hard.future_value, soft.future_value);
    candidate.strategy_value =
        bounded_enemy_mix(hard.strategy_value, soft.strategy_value);
    candidate.moved_steps = hard.stats.moved_steps < soft.stats.moved_steps
                                ? hard.stats.moved_steps
                                : soft.stats.moved_steps;
    candidate.hard_final_gold = hard.final_gold;
    candidate.soft_final_gold = soft.final_gold;
    return candidate;
}

long long reachable_gold_potential(const Model& model,
                                   const Simulation& simulation,
                                   const int actor, const int remaining_steps) {
    if (remaining_steps <= 0) {
        return 0;
    }
    const Position origin = simulation.positions[actor];
    long long value = 0;
    for (int index = 0; index < model.dynamic_gold_count; ++index) {
        const Position position = model.dynamic_gold_positions[index];
        const int row = position.row;
        const int col = position.col;
        const int distance = absolute_difference(origin.row, row) +
                             absolute_difference(origin.col, col);
        if (distance <= 0 || distance > remaining_steps ||
            model.terrain[row][col] == TERRAIN_WALL) {
            continue;
        }
        const int amount = dynamic_tile_at(model, simulation, row, col);
            if (amount > 0) {
                value += pickup_amount(amount) * 10000 / (distance + 1);
            }
    }
    return value;
}

long long path_node_rank(const Model& model, const PathNode& node,
                         const int actor, const int depth) {
    const Position endpoint = node.simulation.positions[actor];
    const long long net_delta =
        node.simulation.gold[actor] - model.starting_gold[actor];
    long long value = net_delta * 1000000;
    value += node.stats.collected * 5000;
    value += remaining_gold_value(model, node.simulation, actor,
                                  OUR_TURN_SECOND) * 80;
    value += reachable_gold_potential(model, node.simulation, actor,
                                      S - depth);

    if (actor == model.miner_actor) {
        value += miner_endpoint_value(model, endpoint) * 30;
        value += explorer_endpoint_value(model, endpoint) * 4;
    } else {
        value += explorer_endpoint_value(model, endpoint) * 30;
        value += miner_endpoint_value(model, endpoint) * 4;
    }
    // Progress is bounded to +/-4000 heat points, hence this beam-only term is
    // bounded to +/-48000 rank points and remains far below one collected coin
    // in the exact-economic portion of this preliminary rank.
    value += region_progress_value(model, model.starts[actor], endpoint) * 12;

    value -= node.stats.bomb_loss * 1000000;
    value -= node.stats.trample_loss * 1000000;
    value -= node.stats.enemy_competition_penalty * 200000;
    value -= node.stats.unknown_path_penalty * 100;
    value -= node.stats.stale_hazard_penalty * 200;
    value -= node.stats.enemy_path_penalty * 160;
    value -= node.stats.enemy_endpoint_penalty * 80;
    value += node.stats.moved_steps;
    return value;
}

bool path_node_better(const PathNode& lhs, const PathNode& rhs) {
    if (lhs.rank != rhs.rank) {
        return lhs.rank > rhs.rank;
    }
    if (lhs.stats.moved_steps != rhs.stats.moved_steps) {
        return lhs.stats.moved_steps > rhs.stats.moved_steps;
    }
    return lexicographically_less(lhs.actions, rhs.actions);
}

int best_unselected_path(const PathNode candidates[PATH_EXPANSION_COUNT],
                         const int candidate_count,
                         const bool selected[PATH_EXPANSION_COUNT],
                         const bool endpoint_used[GRID_SIZE][GRID_SIZE],
                         const bool require_new_endpoint,
                         const int actor) {
    int best = -1;
    for (int index = 0; index < candidate_count; ++index) {
        if (selected[index]) {
            continue;
        }
        const Position endpoint = candidates[index].simulation.positions[actor];
        if (require_new_endpoint && endpoint_used[endpoint.row][endpoint.col]) {
            continue;
        }
        if (best < 0 || path_node_better(candidates[index], candidates[best])) {
            best = index;
        }
    }
    return best;
}

void save_path_set(const PathNode nodes[PATH_BEAM_WIDTH], const int count,
                   PathSet& output) {
    output.count = count;
    for (int index = 0; index < count; ++index) {
        std::memcpy(output.options[index].actions, nodes[index].actions,
                    sizeof(output.options[index].actions));
    }
}

[[maybe_unused]] void generate_actor_paths(const Model& model, const int actor,
                                           PathSet output[S + 1]) {
    PathNode beam[PATH_BEAM_WIDTH]{};
    int beam_count = 1;
    for (int index = 0; index < S; ++index) {
        beam[0].actions[index] = 4;
    }
    beam[0].simulation = make_simulation(model);
    // Candidate generation intentionally ignores the teammate's current
    // occupancy.  The exact joint evaluation below restores both actors and
    // checks execution order/collisions, so paths that become legal after the
    // teammate moves are not pruned prematurely.
    beam[0].simulation.positions[1 - actor] = Position{-1, -1};
    beam[0].rank = path_node_rank(model, beam[0], actor, 0);
    save_path_set(beam, beam_count, output[0]);

    for (int depth = 1; depth <= S; ++depth) {
        PathNode expanded[PATH_EXPANSION_COUNT]{};
        int expanded_count = 0;
        for (int index = 0; index < beam_count; ++index) {
            for (int action = 0; action < ACTION_COUNT; ++action) {
                PathNode& node = expanded[expanded_count++];
                node = beam[index];
                node.actions[depth - 1] = action;
                execute_step(model, node.simulation, actor, action, depth - 1,
                             node.stats, OUR_TURN_SECOND);
                node.rank = path_node_rank(model, node, actor, depth);
            }
        }

        PathNode next[PATH_BEAM_WIDTH]{};
        bool selected[PATH_EXPANSION_COUNT]{};
        bool endpoint_used[GRID_SIZE][GRID_SIZE]{};
        int next_count = 0;
        while (next_count < PATH_DIVERSE_ENDPOINTS &&
               next_count < expanded_count) {
            const int best = best_unselected_path(
                expanded, expanded_count, selected, endpoint_used, true, actor);
            if (best < 0) {
                break;
            }
            selected[best] = true;
            const Position endpoint = expanded[best].simulation.positions[actor];
            endpoint_used[endpoint.row][endpoint.col] = true;
            next[next_count++] = expanded[best];
        }
        while (next_count < PATH_BEAM_WIDTH && next_count < expanded_count) {
            const int best = best_unselected_path(
                expanded, expanded_count, selected, endpoint_used, false, actor);
            if (best < 0) {
                break;
            }
            selected[best] = true;
            next[next_count++] = expanded[best];
        }
        std::memcpy(beam, next, sizeof(next));
        beam_count = next_count;
        save_path_set(beam, beam_count, output[depth]);
    }
}

GameOutput fallback_output() {
    GameOutput output{};
    for (int index = 0; index < S; ++index) {
        output.actions[index] = 4;
    }
    output.k = 3;
    output.order = 0;
    output.vp = 0;
    return output;
}

// The fast planner is deliberately independent from Model/path generation.
// It makes at most 14 candidates (k=0..6 and both execution orders), and each
// candidate executes exactly six bounded greedy steps.  The full planner above
// remains the default/reversible path when the macro is zero.
struct FastTarget {
    Position position;
    long long value;
    bool valid;
};

struct FastCandidate {
    int actions[S];
    int k;
    int order;
    long long collected;
    int moved;
    int target_progress;
};

int fast_cell_value(const GameInput& input, const int row, const int col) {
    if (!in_bounds(row, col)) {
        return TERRAIN_WALL;
    }
    const int visible = input.grid[row][col];
    if (visible != TERRAIN_UNKNOWN) {
        return visible;
    }
    if (g_state.terrain[row][col] == TERRAIN_WALL) {
        return TERRAIN_WALL;
    }
    if (g_state.bomb_state[row][col] == BOMB_PRESENT) {
        return -3;
    }
    return TERRAIN_UNKNOWN;
}

bool fast_visible_enemy_at(const GameInput& input, const int row,
                           const int col) {
    for (int index = 0; index < 2; ++index) {
        if (input.visible_enemies[index].row == row &&
            input.visible_enemies[index].col == col) {
            return true;
        }
    }
    const int current_round = input.round < 0 ? 0 : input.round;
    return g_state.enemy_last_seen[row][col] == current_round &&
           g_state.enemy_present[row][col];
}

int fast_npc_count_at(const GameInput& input, const int row, const int col) {
    const int current_round = input.round < 0 ? 0 : input.round;
    if (g_state.npc_last_seen[row][col] == current_round) {
        return g_state.npc_count[row][col];
    }
    int count = 0;
    int limit = clamp_int(input.num_visible_npcs, 0, MAX_NPCS);
    for (int index = 0; index < limit; ++index) {
        if (input.visible_npcs[index].pos.row == row &&
            input.visible_npcs[index].pos.col == col) {
            ++count;
        }
    }
    return count;
}

bool fast_safe_destination(const GameInput& input, const Position& target,
                           const Position positions[2], const int actor) {
    if (!in_bounds(target.row, target.col) ||
        same_position(target, positions[1 - actor])) {
        return false;
    }
    const int cell = fast_cell_value(input, target.row, target.col);
    if (cell == TERRAIN_WALL || cell == -3 ||
        fast_visible_enemy_at(input, target.row, target.col) ||
        fast_npc_count_at(input, target.row, target.col) >= 3) {
        return false;
    }
    return true;
}

void fast_append_action(int actions[5], int& count, const int action) {
    if (action < 0 || action >= ACTION_COUNT) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        if (actions[index] == action) {
            return;
        }
    }
    actions[count++] = action;
}

int fast_action_towards(const Position& current, const Position& target,
                       const int actor, const int local_step,
                       const int round, int preferred[5]) {
    int count = 0;
    const int row_delta = target.row - current.row;
    const int col_delta = target.col - current.col;
    // Alternate the primary axis only when both choices are equally useful;
    // the actor/round tie-break is fixed and makes route shape deterministic.
    const bool row_first = ((actor + local_step + round) & 1) == 0;
    const int row_action = row_delta < 0 ? 0 : (row_delta > 0 ? 1 : -1);
    const int col_action = col_delta < 0 ? 2 : (col_delta > 0 ? 3 : -1);
    if (row_first) {
        fast_append_action(preferred, count, row_action);
        fast_append_action(preferred, count, col_action);
    } else {
        fast_append_action(preferred, count, col_action);
        fast_append_action(preferred, count, row_action);
    }
    // If a direct step is blocked, try the other axes in a stable order.  The
    // caller falls back to wait after all movement options are rejected.
    fast_append_action(preferred, count, 0);
    fast_append_action(preferred, count, 1);
    fast_append_action(preferred, count, 2);
    fast_append_action(preferred, count, 3);
    return count;
}

Position fast_exploration_target(const GameInput& input, const int actor) {
    Position target = input.my_units[actor];
    const int direction = ((clamp_int(input.round, 0, VISION_LAST_ROUND) / S) +
                           actor) & 3;
    for (int step = 0; step < S; ++step) {
        const Position next{target.row + DR[direction],
                             target.col + DC[direction]};
        if (!in_bounds(next.row, next.col)) {
            break;
        }
        target = next;
    }
    return target;
}

long long fast_target_collection_estimate(const int amount,
                                           const int distance) {
    if (amount <= 0 || distance > S) {
        return 0;
    }
    // The first visit happens after the Manhattan approach.  Every additional
    // visit needs a bounded leave/return pair; reuse the exact integer pickup
    // rule for at most S total visits so the estimate cannot overflow or loop
    // on malformed large piles.
    const int visits = distance > 0 ? 1 + (S - distance) / 2 : S / 2;
    int remaining = amount;
    long long collected = 0;
    for (int visit = 0; visit < visits && remaining > 0; ++visit) {
        const int pickup = static_cast<int>(pickup_amount(remaining));
        collected += pickup;
        remaining -= pickup;
    }
    return collected;
}

FastTarget fast_find_target(const GameInput& input, const int actor) {
    FastTarget result{input.my_units[actor], 0, false};
    const Position origin = input.my_units[actor];
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int amount = input.grid[row][col];
            if (amount <= 0) {
                continue;
            }
            if (fast_visible_enemy_at(input, row, col) ||
                fast_npc_count_at(input, row, col) >= 3) {
                continue;
            }
            const int distance = absolute_difference(origin.row, row) +
                                 absolute_difference(origin.col, col);
            if (distance > S) {
                continue;
            }
            const long long score =
                fast_target_collection_estimate(amount, distance) * 1000LL -
                distance * 17LL;
            if (!result.valid || score > result.value ||
                (score == result.value &&
                 (row < result.position.row ||
                  (row == result.position.row && col < result.position.col)))) {
                result.position = Position{row, col};
                result.value = score;
                result.valid = true;
            }
        }
    }
    return result;
}

int fast_choose_action(const GameInput& input, const Position positions[2],
                       const int actor, const Position& target,
                       const int local_step) {
    int preferred[5]{};
    int count = fast_action_towards(positions[actor], target, actor,
                                    local_step, input.round, preferred);
    for (int index = 0; index < count; ++index) {
        if (fast_safe_destination(
                input,
                Position{positions[actor].row + DR[preferred[index]],
                         positions[actor].col + DC[preferred[index]]},
                positions, actor)) {
            return preferred[index];
        }
    }
    return 4;
}

void fast_collect_at(const GameInput& input, const Position& position,
                     Position visited[2 * S], int remaining[2 * S],
                     int& visited_count, FastCandidate& candidate) {
    const int cell = input.grid[position.row][position.col];
    if (cell <= 0) {
        return;
    }
    int found = -1;
    for (int index = 0; index < visited_count; ++index) {
        if (same_position(visited[index], position)) {
            found = index;
            break;
        }
    }
    if (found < 0 && visited_count < 2 * S) {
        found = visited_count++;
        visited[found] = position;
        remaining[found] = cell;
    }
    if (found >= 0 && remaining[found] > 0) {
        const int pickup = static_cast<int>(pickup_amount(remaining[found]));
        candidate.collected += pickup;
        remaining[found] -= pickup;
    }
}

void fast_build_candidate(const GameInput& input,
                          const FastTarget targets[2], const int k,
                          const int order, FastCandidate& candidate) {
    candidate = FastCandidate{};
    candidate.k = k;
    candidate.order = order;
    for (int index = 0; index < S; ++index) {
        candidate.actions[index] = 4;
    }

    Position positions[2] = {input.my_units[0], input.my_units[1]};
    Position visited[2 * S]{};
    int remaining[2 * S]{};
    int visited_count = 0;
    const int lengths[2] = {k, S - k};
    const int actors[2] = {order, 1 - order};
    for (int sequence = 0; sequence < 2; ++sequence) {
        const int actor = actors[sequence];
        const Position exploration = fast_exploration_target(input, actor);
        const Position target = targets[actor].valid
                                    ? targets[actor].position
                                    : exploration;
        const int initial_distance =
            absolute_difference(positions[actor].row, target.row) +
            absolute_difference(positions[actor].col, target.col);
        for (int local_step = 0; local_step < lengths[actor]; ++local_step) {
            const int action = fast_choose_action(
                input, positions, actor, target, local_step);
            const int output_index = actor == 0 ? local_step : k + local_step;
            candidate.actions[output_index] = action;
            const Position next{positions[actor].row + DR[action],
                                positions[actor].col + DC[action]};
            if (action != 4 && fast_safe_destination(input, next, positions,
                                                     actor)) {
                positions[actor] = next;
                ++candidate.moved;
                fast_collect_at(input, positions[actor], visited, remaining,
                                visited_count, candidate);
            }
        }
        const int final_distance =
            absolute_difference(positions[actor].row, target.row) +
            absolute_difference(positions[actor].col, target.col);
        candidate.target_progress += initial_distance - final_distance;
    }
}

bool fast_candidate_better(const FastCandidate& candidate,
                           const FastCandidate& best, const bool found) {
    if (!found) {
        return true;
    }
    if (candidate.collected != best.collected) {
        return candidate.collected > best.collected;
    }
    if (candidate.moved != best.moved) {
        return candidate.moved > best.moved;
    }
    if (candidate.target_progress != best.target_progress) {
        return candidate.target_progress > best.target_progress;
    }
    const int candidate_balance = candidate.k > 3 ? candidate.k - 3 : 3 - candidate.k;
    const int best_balance = best.k > 3 ? best.k - 3 : 3 - best.k;
    if (candidate_balance != best_balance) {
        return candidate_balance < best_balance;
    }
    if (candidate.order != best.order) {
        return candidate.order < best.order;
    }
    return lexicographically_less(candidate.actions, best.actions);
}

[[maybe_unused]] GameOutput fast_plan_after_observe(const GameInput* input) {
    FastTarget targets[2]{};
    targets[0] = fast_find_target(*input, 0);
    targets[1] = fast_find_target(*input, 1);

    FastCandidate best{};
    bool found = false;
    for (int k = 0; k <= S; ++k) {
        for (int order = 0; order <= 1; ++order) {
            FastCandidate candidate{};
            fast_build_candidate(*input, targets, k, order, candidate);
            if (fast_candidate_better(candidate, best, found)) {
                best = candidate;
                found = true;
            }
        }
    }
    if (!found) {
        return fallback_output();
    }
    GameOutput output{};
    std::memcpy(output.actions, best.actions, sizeof(output.actions));
    output.k = best.k;
    output.order = best.order;
    output.vp = 0;
    return output;
}

#if GOLDRUSH_TURBO_PLANNER

// PR21 Turbo: a fixed, stateless Center-Lanes route.  Dynamic gold is read
// only from each actor's current 5x5 view; all route state is stack-local.
struct TurboTarget { Position position; long long score; bool valid; };

int turbo_visible_npc_count(const GameInput& input, const int row,
                            const int col) {
    if (input.num_visible_npcs <= 0) return 0;
    int count = 0;
    const int limit = clamp_int(input.num_visible_npcs, 0, MAX_NPCS);
    for (int i = 0; i < limit; ++i)
        count += input.visible_npcs[i].pos.row == row &&
                 input.visible_npcs[i].pos.col == col;
    return count;
}

bool turbo_visible_enemy_at(const GameInput& input, const int row,
                            const int col) {
    return (input.visible_enemies[0].row == row &&
            input.visible_enemies[0].col == col) ||
           (input.visible_enemies[1].row == row &&
            input.visible_enemies[1].col == col);
}

bool turbo_safe(const GameInput& input, const Position& next,
                const Position positions[2], const int actor) {
    if (!in_bounds(next.row, next.col) || same_position(next, positions[1 - actor]))
        return false;
    const int cell = input.grid[next.row][next.col];
    return cell != TERRAIN_WALL && cell != -3 &&
           !turbo_visible_enemy_at(input, next.row, next.col) &&
           turbo_visible_npc_count(input, next.row, next.col) < 3;
}

TurboTarget turbo_find_target(const GameInput& input, const int actor) {
    TurboTarget best{input.my_units[actor], 0, false};
    const Position origin = input.my_units[actor];
    const int r0 = origin.row > VIEW_RADIUS ? origin.row - VIEW_RADIUS : 0;
    const int r1 = origin.row + VIEW_RADIUS < GRID_SIZE
                       ? origin.row + VIEW_RADIUS : GRID_SIZE - 1;
    const int c0 = origin.col > VIEW_RADIUS ? origin.col - VIEW_RADIUS : 0;
    const int c1 = origin.col + VIEW_RADIUS < GRID_SIZE
                       ? origin.col + VIEW_RADIUS : GRID_SIZE - 1;
    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            const int amount = input.grid[row][col];
            if (amount <= 0 || turbo_visible_enemy_at(input, row, col) ||
                turbo_visible_npc_count(input, row, col) >= 3)
                continue;
            const int distance = absolute_difference(origin.row, row) +
                                 absolute_difference(origin.col, col);
            const long long score = static_cast<long long>(amount) * 100 - distance;
            if (!best.valid || score > best.score ||
                (score == best.score &&
                 (row < best.position.row ||
                  (row == best.position.row && col < best.position.col))))
                best = TurboTarget{Position{row, col}, score, true};
        }
    }
    return best;
}

int turbo_axis_action(const Position& current, const Position& target,
                      const bool row_first) {
    const int row = target.row == current.row ? -1 : (target.row < current.row ? 0 : 1);
    const int col = target.col == current.col ? -1 : (target.col < current.col ? 2 : 3);
    return row_first ? (row >= 0 ? row : col) : (col >= 0 ? col : row);
}

int turbo_leave_action(const GameInput& input, const Position& current,
                       const int actor, const int phase,
                       const Position positions[2]) {
    const int preferred[4] = {(actor + phase) & 1 ? 3 : 1,
                              (actor + phase) & 1 ? 0 : 2,
                              (actor + phase) & 1 ? 2 : 0,
                              (actor + phase) & 1 ? 1 : 3};
    for (const int action : preferred) {
        const Position next{current.row + DR[action], current.col + DC[action]};
        if (turbo_safe(input, next, positions, actor)) return action;
    }
    return 4;
}

int turbo_step(const GameInput& input, const Position& current,
               const Position& target, const int actor, const int local_step,
               const int phase, const Position positions[2]) {
    if (same_position(current, target))
        return turbo_leave_action(input, current, actor, phase, positions);
    const bool row_first = ((input.round < 0 ? 0 : input.round) + actor +
                            local_step) % 2 == 0;
    const int first = turbo_axis_action(current, target, row_first);
    const int second = turbo_axis_action(current, target, !row_first);
    const int fallback[4] = {0, 1, 2, 3};
    const int choices[6] = {first, second, fallback[0], fallback[1], fallback[2], fallback[3]};
    for (const int action : choices) {
        if (action < 0 || action >= 4) continue;
        const Position next{current.row + DR[action], current.col + DC[action]};
        if (turbo_safe(input, next, positions, actor)) return action;
    }
    return 4;
}

GameOutput turbo_plan_from_targets(const GameInput* input,
                                   const TurboTarget targets[2]) {
    if (!valid_unit_layout(input)) return fallback_output();
    static constexpr Position lanes[2][4] = {
        {{8, 6}, {8, 10}, {10, 10}, {10, 6}},
        {{6, 6}, {6, 10}, {8, 10}, {8, 6}}};
    Position positions[2] = {input->my_units[0], input->my_units[1]};
    GameOutput output{};
    output.k = 3; output.order = 0; output.vp = 0;
    for (int actor = 0; actor < 2; ++actor) {
        const int round = input->round < 0 ? 0 : input->round;
        int phase = (round + actor) & 3;
        Position target = targets[actor].valid ? targets[actor].position : lanes[actor][phase];
        const int begin = actor == 0 ? 0 : 3;
        for (int step = 0; step < 3; ++step) {
            if (!targets[actor].valid && same_position(positions[actor], target)) {
                phase = (phase + 1) & 3;
                target = lanes[actor][phase];
            }
            const int action = turbo_step(*input, positions[actor], target,
                                          actor, step, phase, positions);
            output.actions[begin + step] = action;
            if (action != 4) {
                positions[actor].row += DR[action];
                positions[actor].col += DC[action];
            }
        }
    }
    return output;
}

GameOutput turbo_plan_stateless(const GameInput* input) {
    if (!valid_unit_layout(input)) return fallback_output();
    const TurboTarget targets[2] = {turbo_find_target(*input, 0),
                                    turbo_find_target(*input, 1)};
    return turbo_plan_from_targets(input, targets);
}
#endif

[[maybe_unused]] GameOutput plan(const GameInput* input) {
    // Reject an invalid unit layout before any persistent observation.  In
    // particular, do not feed arbitrary coordinates to reset heuristics or
    // let a malformed host call corrupt the next valid round's memory.
    if (!valid_unit_layout(input)) {
        return fallback_output();
    }
#if GOLDRUSH_TURBO_PLANNER
    return turbo_plan_stateless(input);
#else
    observe_input(input);
#if GOLDRUSH_FAST_PLANNER
    return fast_plan_after_observe(input);
#else
    const Model model = make_model_with_memory(input, &g_state);
    if (!model.valid) {
        return fallback_output();
    }

    PathSet paths[2][S + 1]{};
    generate_actor_paths(model, 0, paths[0]);
    generate_actor_paths(model, 1, paths[1]);

    Candidate best{};
    bool found = false;
    for (int k = 0; k <= S; ++k) {
        const PathSet& first_paths = paths[0][k];
        const PathSet& second_paths = paths[1][S - k];
        for (int first_index = 0; first_index < first_paths.count;
             ++first_index) {
            for (int second_index = 0; second_index < second_paths.count;
                 ++second_index) {
                int actions[S]{};
                for (int index = 0; index < k; ++index) {
                    actions[index] =
                        first_paths.options[first_index].actions[index];
                }
                for (int index = k; index < S; ++index) {
                    actions[index] = second_paths.options[second_index]
                                         .actions[index - k];
                }

                // With one actor receiving all six actions, the other actor
                // is idle; swapping execution order is a strict duplicate.
                const int order_count = (k == 0 || k == S) ? 1 : 2;
                for (int order = 0; order < order_count; ++order) {
                    const Candidate candidate =
                        evaluate_candidate(model, actions, k, order);
                    if (!found || better_candidate(candidate, best)) {
                        best = candidate;
                        found = true;
                    }
                }
            }
        }
    }

    if (!found) {
        return fallback_output();
    }

    GameOutput output{};
    std::memcpy(output.actions, best.actions, sizeof(output.actions));
    output.k = best.k;
    output.order = best.order;
    output.vp = 0;
    return output;
#endif
#endif
}

}  // namespace

extern "C" GameOutput moveDecision(const GameInput* input) {
#if GOLDRUSH_TURBO_PLANNER
    return turbo_plan_stateless(input);
#else
    try {
        GameOutput output = plan(input);
        if (!valid_unit_layout(input)) {
            return output;
        }
        const int round = clamp_int(input->round, 0, VISION_LAST_ROUND);
        // Hosts and tests may repeat a decision call for the same round.  The
        // byte-identical cached vp avoids charging the per-match budget twice.
        if (g_state.vision_last_decision_round == round) {
            output.vp = g_state.vision_last_decision_vp;
            return output;
        }
        output.vp = choose_vision_purchase(input, g_state);
        g_state.vision_last_decision_round = round;
        g_state.vision_last_decision_vp = output.vp;
        if (output.vp == 1 || output.vp == 2) {
            const int cost = output.vp == 1 ? VP1_COST : VP2_COST;
            g_state.vision_spent += cost;
            g_state.vision_last_purchase_round = round;
        }
        return output;
    } catch (...) {
        return fallback_output();
    }
#endif
}
