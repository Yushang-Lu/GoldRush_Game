#include "game_api.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr int kCells = GRID_SIZE * GRID_SIZE;
constexpr int kStay = 4;
constexpr int kBeamWidth = 6;
constexpr int kMaxTargets = 12;
constexpr int kUnknown = 0;
constexpr int kPassable = 1;
constexpr int kWall = 2;
constexpr std::int64_t kBadScore =
    std::numeric_limits<std::int64_t>::min() / 4;

constexpr int kDr[4] = {-1, 1, 0, 0};
constexpr int kDc[4] = {0, 0, -1, 1};

struct PersistentState {
    bool initialized;
    int last_round;
    int last_purchase_round;
    int vision_spend;
    std::uint8_t terrain[kCells];
    int remembered_gold[kCells];
    int gold_seen_round[kCells];
    int last_seen_round[kCells];
    int last_visit_round[kCells];
    int region_score[REGION_COUNT + 1];
};

PersistentState g_state{};

struct Target {
    int cell;
    int gold;
    int priority;
};

struct TurnContext {
    const GameInput* input;
    std::uint8_t npc_count[kCells];
    std::uint8_t enemy_occupied[kCells];
    int position_value[kCells];
    Target targets[kMaxTargets];
    int target_count;
};

struct Candidate {
    std::uint8_t actions[S];
    std::int64_t score;
};

struct CandidateSet {
    Candidate items[kBeamWidth];
    int count;
};

struct SearchNode {
    int pos;
    std::int64_t gold;
    std::int64_t rank;
    int path_adjustment;
    int blocked;
    std::uint8_t actions[S];
    int changed_cells[S];
    int gold_left[S];
    int changed_count;
    int consumed_bombs[S];
    int consumed_count;
    int visited[S + 1];
    int visited_count;
};

struct Simulation {
    int pos[2];
    std::int64_t gold[2];
    int changed_cells[S];
    int gold_left[S];
    int changed_count;
    int consumed_bombs[S];
    int consumed_count;
    int visited[2][S + 1];
    int visited_count[2];
    int path_adjustment;
    bool valid;
};

struct Plan {
    GameOutput output;
    std::int64_t score;
    bool valid;
};

inline int clampInt(int value, int low, int high) noexcept {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

inline bool validPosition(const Position& pos) noexcept {
    return pos.row >= 0 && pos.row < GRID_SIZE && pos.col >= 0 &&
           pos.col < GRID_SIZE;
}

inline int cellIndex(int row, int col) noexcept {
    return row * GRID_SIZE + col;
}

inline int rowOf(int cell) noexcept {
    return cell / GRID_SIZE;
}

inline int colOf(int cell) noexcept {
    return cell % GRID_SIZE;
}

inline int manhattan(int lhs, int rhs) noexcept {
    int dr = rowOf(lhs) - rowOf(rhs);
    int dc = colOf(lhs) - colOf(rhs);
    if (dr < 0) {
        dr = -dr;
    }
    if (dc < 0) {
        dc = -dc;
    }
    return dr + dc;
}

GameOutput fallbackOutput() noexcept {
    GameOutput output;
    for (int i = 0; i < S; ++i) {
        output.actions[i] = kStay;
    }
    output.k = 0;
    output.order = 0;
    output.vp = 0;
    return output;
}

void resetState() noexcept {
    g_state.initialized = true;
    g_state.last_round = -1;
    g_state.last_purchase_round = -1000;
    g_state.vision_spend = 0;
    std::memset(g_state.terrain, kUnknown, sizeof(g_state.terrain));
    std::memset(g_state.remembered_gold, 0,
                sizeof(g_state.remembered_gold));
    for (int cell = 0; cell < kCells; ++cell) {
        g_state.gold_seen_round[cell] = -1000;
        g_state.last_seen_round[cell] = -1000;
        g_state.last_visit_round[cell] = -1000;
    }
    for (int region = 0; region <= REGION_COUNT; ++region) {
        g_state.region_score[region] = 0;
    }
}

int regionOf(int cell) noexcept {
    const int row = rowOf(cell);
    const int col = colOf(cell);
    if (row >= 4 && row <= 12 && col >= 4 && col <= 12) {
        return 1;
    }
    if (row <= 3 && col <= 12) {
        return 2;
    }
    if (col <= 3 && row >= 4) {
        return 3;
    }
    if (row >= 13 && col >= 4) {
        return 4;
    }
    return 5;
}

void updateSnapshot(const GameInput& input) noexcept {
    if (input.snapshot_valid != 1) {
        return;
    }

    int next_scores[REGION_COUNT + 1] = {};
    bool found = false;
    for (int i = 0; i < REGION_COUNT; ++i) {
        const RegionStat& stat = input.snapshot.regions[i];
        if (stat.id < 1 || stat.id > REGION_COUNT) {
            continue;
        }
        const int remaining = clampInt(stat.gold_remaining, 0, 1000000);
        const int generated = clampInt(stat.gold_generated, 0, 1000000);
        const int collected = clampInt(stat.gold_collected, 0, 1000000);
        const int occupants = clampInt(stat.occupants, 0, 100);
        const int area = stat.id == 1 ? 81 : 52;
        const std::int64_t unclaimed =
            generated > collected ? generated - collected : 0;
        std::int64_t score =
            (static_cast<std::int64_t>(remaining) * 10 + unclaimed * 2) /
                area -
            static_cast<std::int64_t>(occupants) * 4;
        score = clampInt(static_cast<int>(score), -40, 120);
        next_scores[stat.id] = static_cast<int>(score);
        found = true;
    }
    if (found) {
        for (int region = 1; region <= REGION_COUNT; ++region) {
            g_state.region_score[region] = next_scores[region];
        }
    }
}

void updateMemory(const GameInput& input) noexcept {
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int cell = cellIndex(row, col);
            const int value = input.grid[row][col];
            if (value == -5) {
                continue;
            }
            g_state.last_seen_round[cell] = input.round;
            if (value == -1) {
                g_state.terrain[cell] = kWall;
                g_state.remembered_gold[cell] = 0;
                g_state.gold_seen_round[cell] = input.round;
            } else if (value == -3 || value >= 0) {
                g_state.terrain[cell] = kPassable;
                g_state.remembered_gold[cell] =
                    value > 0 ? clampInt(value, 0, 1000000) : 0;
                g_state.gold_seen_round[cell] = input.round;
            }
        }
    }

    for (int unit = 0; unit < 2; ++unit) {
        if (validPosition(input.my_units[unit])) {
            const Position& pos = input.my_units[unit];
            g_state.last_visit_round[cellIndex(pos.row, pos.col)] = input.round;
        }
    }
    updateSnapshot(input);
    g_state.last_round = input.round;
}

void prepareState(const GameInput& input) noexcept {
    const bool discontinuous =
        g_state.initialized && g_state.last_round >= 0 &&
        input.round != g_state.last_round + 1;
    if (!g_state.initialized || input.round == 0 || discontinuous) {
        resetState();
    }
    updateMemory(input);
}

inline bool isWall(const TurnContext& context, int cell) noexcept {
    const int value = context.input->grid[rowOf(cell)][colOf(cell)];
    return value == -1 || g_state.terrain[cell] == kWall;
}

int estimatedGold(const TurnContext& context, int cell) noexcept {
    const int current = context.input->grid[rowOf(cell)][colOf(cell)];
    if (current >= 1) {
        return clampInt(current, 1, 1000000);
    }
    if (current != -5) {
        return 0;
    }

    const int remembered = g_state.remembered_gold[cell];
    const int age = context.input->round - g_state.gold_seen_round[cell];
    if (remembered <= 0 || age < 0 || age > 8) {
        return 0;
    }
    if (age <= 2) {
        return remembered;
    }
    if (age <= 5) {
        return (remembered + 1) / 2;
    }
    return (remembered + 3) / 4;
}

inline int pickupAmount(int remaining) noexcept {
    if (remaining <= 0) {
        return 0;
    }
    return static_cast<int>((static_cast<std::int64_t>(remaining) * 65 + 99) /
                            100);
}

inline std::int64_t bombLoss(std::int64_t held) noexcept {
    return held <= 0 ? 0 : (held + 9) / 10;
}

inline std::int64_t stampedeLoss(std::int64_t held) noexcept {
    return held <= 0 ? 0 : (held + 19) / 20;
}

void insertTarget(TurnContext& context, const Target& target) noexcept {
    int insertion = context.target_count;
    if (insertion < kMaxTargets) {
        ++context.target_count;
    } else {
        if (context.targets[kMaxTargets - 1].priority >= target.priority) {
            return;
        }
        insertion = kMaxTargets - 1;
    }

    while (insertion > 0 &&
           context.targets[insertion - 1].priority < target.priority) {
        if (insertion < kMaxTargets) {
            context.targets[insertion] = context.targets[insertion - 1];
        }
        --insertion;
    }
    context.targets[insertion] = target;
}

void buildContext(const GameInput& input, TurnContext& context) noexcept {
    context.input = &input;
    context.target_count = 0;
    std::memset(context.npc_count, 0, sizeof(context.npc_count));
    std::memset(context.enemy_occupied, 0, sizeof(context.enemy_occupied));

    for (int enemy = 0; enemy < 2; ++enemy) {
        const Position& pos = input.visible_enemies[enemy];
        if (validPosition(pos)) {
            context.enemy_occupied[cellIndex(pos.row, pos.col)] = 1;
        }
    }

    const int npc_count = clampInt(input.num_visible_npcs, 0, MAX_NPCS);
    for (int npc = 0; npc < npc_count; ++npc) {
        const NpcInfo& info = input.visible_npcs[npc];
        if (info.id == 0 || !validPosition(info.pos)) {
            continue;
        }
        const int cell = cellIndex(info.pos.row, info.pos.col);
        if (context.npc_count[cell] < 255) {
            ++context.npc_count[cell];
        }
    }

    for (int cell = 0; cell < kCells; ++cell) {
        const int gold = estimatedGold(context, cell);
        if (gold <= 0 || isWall(context, cell)) {
            continue;
        }
        const int age = input.round - g_state.gold_seen_round[cell];
        int priority = gold * 16;
        if (age <= 0) {
            priority += 32;
        } else {
            priority -= age * 3;
        }
        if (context.enemy_occupied[cell] != 0) {
            priority /= 2;
        }
        insertTarget(context, Target{cell, gold, priority});
    }
}

int knowledgeValue(const TurnContext& context, int cell) noexcept {
    const int center_row = rowOf(cell);
    const int center_col = colOf(cell);
    int value = 0;
    for (int dr = -2; dr <= 2; ++dr) {
        const int row = center_row + dr;
        if (row < 0 || row >= GRID_SIZE) {
            continue;
        }
        for (int dc = -2; dc <= 2; ++dc) {
            const int col = center_col + dc;
            if (col < 0 || col >= GRID_SIZE) {
                continue;
            }
            const int seen = g_state.last_seen_round[cellIndex(row, col)];
            const int age = context.input->round - seen;
            if (seen < 0) {
                value += 4;
            } else if (age > 12) {
                value += 2;
            } else if (age > 5) {
                value += 1;
            }
        }
    }
    return value;
}

int calculatePositionValue(const TurnContext& context, int cell) noexcept {
    int value = knowledgeValue(context, cell);
    const int row = rowOf(cell);
    const int col = colOf(cell);
    if (row >= 4 && row <= 12 && col >= 4 && col <= 12) {
        value += 14;
    }
    value += g_state.region_score[regionOf(cell)];

    const int last_visit = g_state.last_visit_round[cell];
    const int visit_age = context.input->round - last_visit;
    if (last_visit < 0) {
        value += 18;
    } else if (visit_age <= 2) {
        value -= 18;
    } else if (visit_age > 20) {
        value += 8;
    }

    if (context.npc_count[cell] >= 3) {
        value -= 35;
    }
    for (int enemy = 0; enemy < 2; ++enemy) {
        const Position& enemy_pos = context.input->visible_enemies[enemy];
        if (!validPosition(enemy_pos)) {
            continue;
        }
        const int enemy_cell = cellIndex(enemy_pos.row, enemy_pos.col);
        const int distance = manhattan(cell, enemy_cell);
        if (distance == 1) {
            value -= 45;
        } else if (distance == 2) {
            value -= 15;
        }
    }
    return value;
}

void cachePositionValues(TurnContext& context) noexcept {
    for (int cell = 0; cell < kCells; ++cell) {
        context.position_value[cell] = calculatePositionValue(context, cell);
    }
}

inline int positionValue(const TurnContext& context, int cell) noexcept {
    return context.position_value[cell];
}

int nodeGoldLeft(const TurnContext& context, const SearchNode& node,
                 int cell) noexcept {
    for (int i = 0; i < node.changed_count; ++i) {
        if (node.changed_cells[i] == cell) {
            return node.gold_left[i];
        }
    }
    return estimatedGold(context, cell);
}

void setNodeGold(SearchNode& node, int cell, int remaining) noexcept {
    for (int i = 0; i < node.changed_count; ++i) {
        if (node.changed_cells[i] == cell) {
            node.gold_left[i] = remaining;
            return;
        }
    }
    if (node.changed_count < S) {
        node.changed_cells[node.changed_count] = cell;
        node.gold_left[node.changed_count] = remaining;
        ++node.changed_count;
    }
}

bool nodeBombConsumed(const SearchNode& node, int cell) noexcept {
    for (int i = 0; i < node.consumed_count; ++i) {
        if (node.consumed_bombs[i] == cell) {
            return true;
        }
    }
    return false;
}

int targetPotential(const TurnContext& context, const SearchNode& node,
                    int pos) noexcept {
    int best = 0;
    for (int i = 0; i < context.target_count; ++i) {
        const Target& target = context.targets[i];
        const int remaining = nodeGoldLeft(context, node, target.cell);
        if (remaining <= 0) {
            continue;
        }
        const int distance = manhattan(pos, target.cell);
        int potential = pickupAmount(remaining) * 650 / (distance + 1);
        for (int enemy = 0; enemy < 2; ++enemy) {
            const Position& enemy_pos = context.input->visible_enemies[enemy];
            if (!validPosition(enemy_pos)) {
                continue;
            }
            const int enemy_cell = cellIndex(enemy_pos.row, enemy_pos.col);
            if (manhattan(enemy_cell, target.cell) < distance) {
                potential = potential * 3 / 4;
                break;
            }
        }
        if (potential > best) {
            best = potential;
        }
    }
    return best;
}

std::int64_t rankNode(const TurnContext& context, const SearchNode& node,
                      std::int64_t initial_gold, int unit) noexcept {
    std::int64_t score = (node.gold - initial_gold) * 1000;
    score += node.path_adjustment;
    score += targetPotential(context, node, node.pos);
    score += positionValue(context, node.pos);
    score -= static_cast<std::int64_t>(node.blocked) * 240;
    const int tie = (node.pos * 13 + unit * 7 + context.input->round * 3) % 11;
    score += tie;
    return score;
}

bool nodeBetter(const SearchNode& lhs, const SearchNode& rhs) noexcept {
    if (lhs.rank != rhs.rank) {
        return lhs.rank > rhs.rank;
    }
    if (lhs.blocked != rhs.blocked) {
        return lhs.blocked < rhs.blocked;
    }
    for (int i = 0; i < S; ++i) {
        if (lhs.actions[i] != rhs.actions[i]) {
            return lhs.actions[i] < rhs.actions[i];
        }
    }
    return false;
}

void insertNode(SearchNode (&nodes)[kBeamWidth], int& count,
                const SearchNode& candidate) noexcept {
    int insertion = count;
    if (insertion < kBeamWidth) {
        ++count;
    } else {
        if (!nodeBetter(candidate, nodes[kBeamWidth - 1])) {
            return;
        }
        insertion = kBeamWidth - 1;
    }
    while (insertion > 0 && nodeBetter(candidate, nodes[insertion - 1])) {
        if (insertion < kBeamWidth) {
            nodes[insertion] = nodes[insertion - 1];
        }
        --insertion;
    }
    nodes[insertion] = candidate;
}

void appendVisited(SearchNode& node, int cell) noexcept {
    if (node.visited_count < S + 1) {
        node.visited[node.visited_count] = cell;
        ++node.visited_count;
    }
}

bool wasVisited(const SearchNode& node, int cell) noexcept {
    for (int i = 0; i < node.visited_count; ++i) {
        if (node.visited[i] == cell) {
            return true;
        }
    }
    return false;
}

SearchNode expandNode(const TurnContext& context, const SearchNode& parent,
                      int action, int depth, std::int64_t initial_gold,
                      int unit) noexcept {
    SearchNode node = parent;
    node.actions[depth] = static_cast<std::uint8_t>(action);

    if (action == kStay) {
        node.path_adjustment -= 10;
        appendVisited(node, node.pos);
        node.rank = rankNode(context, node, initial_gold, unit);
        return node;
    }

    const int next_row = rowOf(node.pos) + kDr[action];
    const int next_col = colOf(node.pos) + kDc[action];
    if (next_row < 0 || next_row >= GRID_SIZE || next_col < 0 ||
        next_col >= GRID_SIZE) {
        ++node.blocked;
        appendVisited(node, node.pos);
        node.rank = rankNode(context, node, initial_gold, unit);
        return node;
    }

    const int next = cellIndex(next_row, next_col);
    if (isWall(context, next) || context.enemy_occupied[next] != 0) {
        ++node.blocked;
        appendVisited(node, node.pos);
        node.rank = rankNode(context, node, initial_gold, unit);
        return node;
    }

    const bool repeated = wasVisited(node, next);
    node.pos = next;
    appendVisited(node, next);
    node.path_adjustment += 3;
    if (repeated) {
        node.path_adjustment -= 18;
    }
    if (context.input->grid[next_row][next_col] == -5) {
        node.path_adjustment += 8;
        node.path_adjustment -=
            static_cast<int>(node.gold > 600 ? 12 : node.gold / 50);
    }

    const int remaining = nodeGoldLeft(context, node, next);
    const int pickup = pickupAmount(remaining);
    if (pickup > 0) {
        node.gold += pickup;
        setNodeGold(node, next, remaining - pickup);
        if (repeated) {
            node.path_adjustment += 12;
        }
    }

    if (context.input->grid[next_row][next_col] == -3 &&
        !nodeBombConsumed(node, next)) {
        node.gold -= bombLoss(node.gold);
        if (node.consumed_count < S) {
            node.consumed_bombs[node.consumed_count] = next;
            ++node.consumed_count;
        }
        node.path_adjustment -= 35;
    }
    if (context.npc_count[next] >= 3) {
        node.gold -= stampedeLoss(node.gold);
        node.path_adjustment -= 25;
    }

    node.rank = rankNode(context, node, initial_gold, unit);
    return node;
}

void saveCandidates(const SearchNode (&nodes)[kBeamWidth], int count,
                    CandidateSet& output) noexcept {
    output.count = count;
    for (int i = 0; i < count; ++i) {
        output.items[i].score = nodes[i].rank;
        for (int step = 0; step < S; ++step) {
            output.items[i].actions[step] = nodes[i].actions[step];
        }
    }
}

void generateCandidates(const TurnContext& context, int unit,
                        CandidateSet (&sets)[S + 1]) noexcept {
    SearchNode current[kBeamWidth] = {};
    SearchNode next[kBeamWidth] = {};
    int current_count = 1;
    current[0].pos = cellIndex(context.input->my_units[unit].row,
                               context.input->my_units[unit].col);
    current[0].gold = clampInt(context.input->my_units_gold[unit], 0,
                               1000000000);
    current[0].visited[0] = current[0].pos;
    current[0].visited_count = 1;
    for (int step = 0; step < S; ++step) {
        current[0].actions[step] = kStay;
    }
    current[0].rank =
        rankNode(context, current[0], current[0].gold, unit);
    saveCandidates(current, current_count, sets[0]);

    const std::int64_t initial_gold = current[0].gold;
    for (int depth = 0; depth < S; ++depth) {
        int next_count = 0;
        for (int i = 0; i < current_count; ++i) {
            for (int action = 0; action <= kStay; ++action) {
                const SearchNode expanded =
                    expandNode(context, current[i], action, depth,
                               initial_gold, unit);
                insertNode(next, next_count, expanded);
            }
        }
        for (int i = 0; i < next_count; ++i) {
            current[i] = next[i];
        }
        current_count = next_count;
        saveCandidates(current, current_count, sets[depth + 1]);
    }
}

int simulationGoldLeft(const TurnContext& context, const Simulation& sim,
                       int cell) noexcept {
    for (int i = 0; i < sim.changed_count; ++i) {
        if (sim.changed_cells[i] == cell) {
            return sim.gold_left[i];
        }
    }
    return estimatedGold(context, cell);
}

void setSimulationGold(Simulation& sim, int cell, int remaining) noexcept {
    for (int i = 0; i < sim.changed_count; ++i) {
        if (sim.changed_cells[i] == cell) {
            sim.gold_left[i] = remaining;
            return;
        }
    }
    if (sim.changed_count < S) {
        sim.changed_cells[sim.changed_count] = cell;
        sim.gold_left[sim.changed_count] = remaining;
        ++sim.changed_count;
    }
}

bool simulationBombConsumed(const Simulation& sim, int cell) noexcept {
    for (int i = 0; i < sim.consumed_count; ++i) {
        if (sim.consumed_bombs[i] == cell) {
            return true;
        }
    }
    return false;
}

bool simulationVisited(const Simulation& sim, int unit, int cell) noexcept {
    for (int i = 0; i < sim.visited_count[unit]; ++i) {
        if (sim.visited[unit][i] == cell) {
            return true;
        }
    }
    return false;
}

void simulateAction(const TurnContext& context, Simulation& sim, int unit,
                    int action) noexcept {
    if (!sim.valid) {
        return;
    }
    if (action == kStay) {
        sim.path_adjustment -= 10;
        return;
    }
    if (action < 0 || action > 3) {
        sim.valid = false;
        return;
    }

    const int next_row = rowOf(sim.pos[unit]) + kDr[action];
    const int next_col = colOf(sim.pos[unit]) + kDc[action];
    if (next_row < 0 || next_row >= GRID_SIZE || next_col < 0 ||
        next_col >= GRID_SIZE) {
        sim.valid = false;
        return;
    }
    const int next = cellIndex(next_row, next_col);
    if (isWall(context, next) || context.enemy_occupied[next] != 0 ||
        next == sim.pos[1 - unit]) {
        sim.valid = false;
        return;
    }

    const bool repeated = simulationVisited(sim, unit, next);
    sim.pos[unit] = next;
    if (sim.visited_count[unit] < S + 1) {
        sim.visited[unit][sim.visited_count[unit]] = next;
        ++sim.visited_count[unit];
    }
    sim.path_adjustment += 3;
    if (repeated) {
        sim.path_adjustment -= 18;
    }
    if (context.input->grid[next_row][next_col] == -5) {
        sim.path_adjustment += 8;
        sim.path_adjustment -=
            static_cast<int>(sim.gold[unit] > 600 ? 12 : sim.gold[unit] / 50);
    }

    const int remaining = simulationGoldLeft(context, sim, next);
    const int pickup = pickupAmount(remaining);
    if (pickup > 0) {
        sim.gold[unit] += pickup;
        setSimulationGold(sim, next, remaining - pickup);
        if (repeated) {
            sim.path_adjustment += 12;
        }
    }

    if (context.input->grid[next_row][next_col] == -3 &&
        !simulationBombConsumed(sim, next)) {
        sim.gold[unit] -= bombLoss(sim.gold[unit]);
        if (sim.consumed_count < S) {
            sim.consumed_bombs[sim.consumed_count] = next;
            ++sim.consumed_count;
        }
        sim.path_adjustment -= 35;
    }
    if (context.npc_count[next] >= 3) {
        sim.gold[unit] -= stampedeLoss(sim.gold[unit]);
        sim.path_adjustment -= 25;
    }
}

int jointTargetPotential(const TurnContext& context,
                         const Simulation& sim) noexcept {
    int total = 0;
    for (int i = 0; i < context.target_count; ++i) {
        const Target& target = context.targets[i];
        const int remaining = simulationGoldLeft(context, sim, target.cell);
        if (remaining <= 0) {
            continue;
        }
        const int distance0 = manhattan(sim.pos[0], target.cell);
        const int distance1 = manhattan(sim.pos[1], target.cell);
        const int nearest = distance0 < distance1 ? distance0 : distance1;
        int potential = pickupAmount(remaining) * 520 / (nearest + 1);
        const int farther = distance0 > distance1 ? distance0 : distance1;
        if (farther <= nearest + 2) {
            potential = potential * 7 / 8;
        }
        total += potential;
        if (total > 5000) {
            return 5000;
        }
    }
    return total;
}

std::int64_t scoreSimulation(const TurnContext& context,
                             const Simulation& sim,
                             const std::int64_t (&initial_gold)[2]) noexcept {
    if (!sim.valid) {
        return kBadScore;
    }
    std::int64_t score =
        (sim.gold[0] + sim.gold[1] - initial_gold[0] - initial_gold[1]) *
        1000;
    score += sim.path_adjustment;
    score += positionValue(context, sim.pos[0]);
    score += positionValue(context, sim.pos[1]);
    score += jointTargetPotential(context, sim);

    const int separation = manhattan(sim.pos[0], sim.pos[1]);
    score += (separation < 7 ? separation : 7) * 7;
    if (separation == 1) {
        score -= 30;
    }
    return score;
}

void initializeSimulation(const TurnContext& context,
                          Simulation& sim) noexcept {
    std::memset(&sim, 0, sizeof(sim));
    for (int unit = 0; unit < 2; ++unit) {
        const Position& pos = context.input->my_units[unit];
        sim.pos[unit] = cellIndex(pos.row, pos.col);
        sim.gold[unit] =
            clampInt(context.input->my_units_gold[unit], 0, 1000000000);
        sim.visited[unit][0] = sim.pos[unit];
        sim.visited_count[unit] = 1;
    }
    sim.valid = sim.pos[0] != sim.pos[1];
}

Plan evaluatePlan(const TurnContext& context, const Candidate& unit0,
                  const Candidate& unit1, int split, int order) noexcept {
    Plan plan;
    plan.output = fallbackOutput();
    plan.output.k = split;
    plan.output.order = order;
    for (int i = 0; i < split; ++i) {
        plan.output.actions[i] = unit0.actions[i];
    }
    for (int i = split; i < S; ++i) {
        plan.output.actions[i] = unit1.actions[i - split];
    }

    Simulation sim;
    initializeSimulation(context, sim);
    const std::int64_t initial_gold[2] = {sim.gold[0], sim.gold[1]};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? order : 1 - order;
        const int begin = unit == 0 ? 0 : split;
        const int end = unit == 0 ? split : S;
        for (int action_index = begin; action_index < end; ++action_index) {
            simulateAction(context, sim, unit,
                           plan.output.actions[action_index]);
        }
    }
    plan.valid = sim.valid;
    plan.score = scoreSimulation(context, sim, initial_gold);
    return plan;
}

bool planBetter(const Plan& lhs, const Plan& rhs) noexcept {
    if (!lhs.valid) {
        return false;
    }
    if (!rhs.valid) {
        return true;
    }
    if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
    }
    int lhs_balance = lhs.output.k - 3;
    int rhs_balance = rhs.output.k - 3;
    if (lhs_balance < 0) {
        lhs_balance = -lhs_balance;
    }
    if (rhs_balance < 0) {
        rhs_balance = -rhs_balance;
    }
    if (lhs_balance != rhs_balance) {
        return lhs_balance < rhs_balance;
    }
    if (lhs.output.order != rhs.output.order) {
        return lhs.output.order < rhs.output.order;
    }
    for (int i = 0; i < S; ++i) {
        if (lhs.output.actions[i] != rhs.output.actions[i]) {
            return lhs.output.actions[i] < rhs.output.actions[i];
        }
    }
    return false;
}

GameOutput searchPlan(const TurnContext& context) noexcept {
    CandidateSet candidates[2][S + 1] = {};
    generateCandidates(context, 0, candidates[0]);
    generateCandidates(context, 1, candidates[1]);

    Plan best;
    best.output = fallbackOutput();
    best.score = kBadScore;
    best.valid = false;
    for (int split = 0; split <= S; ++split) {
        const CandidateSet& set0 = candidates[0][split];
        const CandidateSet& set1 = candidates[1][S - split];
        for (int i = 0; i < set0.count; ++i) {
            for (int j = 0; j < set1.count; ++j) {
                for (int order = 0; order <= 1; ++order) {
                    const Plan candidate = evaluatePlan(
                        context, set0.items[i], set1.items[j], split, order);
                    if (planBetter(candidate, best)) {
                        best = candidate;
                    }
                }
            }
        }
    }
    return best.valid ? best.output : fallbackOutput();
}

void sanitizeKnownCollisions(const TurnContext& context,
                             GameOutput& output) noexcept {
    int positions[2] = {
        cellIndex(context.input->my_units[0].row,
                  context.input->my_units[0].col),
        cellIndex(context.input->my_units[1].row,
                  context.input->my_units[1].col)};
    for (int phase = 0; phase < 2; ++phase) {
        const int unit = phase == 0 ? output.order : 1 - output.order;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int index = begin; index < end; ++index) {
            const int action = output.actions[index];
            if (action == kStay) {
                continue;
            }
            if (action < 0 || action > 3) {
                output.actions[index] = kStay;
                continue;
            }
            const int row = rowOf(positions[unit]) + kDr[action];
            const int col = colOf(positions[unit]) + kDc[action];
            if (row < 0 || row >= GRID_SIZE || col < 0 ||
                col >= GRID_SIZE) {
                output.actions[index] = kStay;
                continue;
            }
            const int next = cellIndex(row, col);
            if (isWall(context, next) ||
                context.enemy_occupied[next] != 0 ||
                next == positions[1 - unit]) {
                output.actions[index] = kStay;
                continue;
            }
            positions[unit] = next;
        }
    }
}

int chooseVisionPurchase(const TurnContext& context) noexcept {
    const GameInput& input = *context.input;
    if (input.round < 2 || input.round >= 490 ||
        input.round - g_state.last_purchase_round < 6) {
        return 0;
    }

    const int own_gold0 = clampInt(input.my_units_gold[0], 0, 1000000000);
    const int own_gold1 = clampInt(input.my_units_gold[1], 0, 1000000000);
    const std::int64_t wealth =
        static_cast<std::int64_t>(own_gold0) + own_gold1;
    if (wealth - g_state.vision_spend < 8) {
        return 0;
    }

    int visible_gold = 0;
    for (int row = 0; row < GRID_SIZE; ++row) {
        for (int col = 0; col < GRID_SIZE; ++col) {
            const int value = input.grid[row][col];
            if (value > 0) {
                visible_gold += clampInt(value, 0, 1000);
                if (visible_gold >= 15) {
                    return 0;
                }
            }
        }
    }

    const int unit0 = cellIndex(input.my_units[0].row, input.my_units[0].col);
    const int unit1 = cellIndex(input.my_units[1].row, input.my_units[1].col);
    const int unknown_value = knowledgeValue(context, unit0) +
                              knowledgeValue(context, unit1);
    const bool behind = input.gold_opp > wealth + 20;
    if (visible_gold > 5 && !behind) {
        return 0;
    }
    if (unknown_value < 12 && context.target_count >= 2) {
        return 0;
    }

    int purchase = 1;
    int best_region = 0;
    for (int region = 1; region <= REGION_COUNT; ++region) {
        if (g_state.region_score[region] > best_region) {
            best_region = g_state.region_score[region];
        }
    }
    if (input.snapshot_valid == 1 && visible_gold == 0 && best_region >= 18 &&
        wealth - g_state.vision_spend >= 20 &&
        input.round - g_state.last_purchase_round >= 10) {
        purchase = 2;
    }

    g_state.last_purchase_round = input.round;
    g_state.vision_spend += purchase == 1 ? 2 : 3;
    return purchase;
}

bool validOwnPositions(const GameInput& input) noexcept {
    return validPosition(input.my_units[0]) && validPosition(input.my_units[1]) &&
           (input.my_units[0].row != input.my_units[1].row ||
            input.my_units[0].col != input.my_units[1].col);
}

}  // namespace

extern "C" GameOutput moveDecision(const GameInput* input) {
    if (input == nullptr || input->round < 0 || input->round > 1000000) {
        return fallbackOutput();
    }

    prepareState(*input);
    if (!validOwnPositions(*input)) {
        return fallbackOutput();
    }

    TurnContext context{};
    buildContext(*input, context);
    cachePositionValues(context);
    GameOutput output = searchPlan(context);
    sanitizeKnownCollisions(context, output);
    output.vp = chooseVisionPurchase(context);

    if (output.k < 0 || output.k > S || output.order < 0 ||
        output.order > 1 || output.vp < 0 || output.vp > 2) {
        return fallbackOutput();
    }
    for (int i = 0; i < S; ++i) {
        if (output.actions[i] < 0 || output.actions[i] > kStay) {
            return fallbackOutput();
        }
    }
    return output;
}
