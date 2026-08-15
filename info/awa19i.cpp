#include "game_api.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace {

constexpr int kStay = 4;
constexpr int kCells = GRID_SIZE * GRID_SIZE;
constexpr std::array<int, 5> kDr{-1, 1, 0, 0, 0};
constexpr std::array<int, 5> kDc{0, 0, -1, 1, 0};
constexpr std::array<int, 4> kReverse{1, 0, 3, 2};
#if defined(GOLD_RUSH_BENCHMARKING) || defined(GOLD_RUSH_TESTING)
bool g_stats = false;
std::uint64_t g_rounds = 0;
#endif
int g_last_vp = -1000;

struct EscapeMemory {
    int last_round = -1000;
    std::array<std::int16_t, 2> last_position{-1, -1};
    std::array<std::uint8_t, 2> final_allstay{0, 0};
    std::array<std::uint8_t, 2> stall_transitions{0, 0};
    std::array<std::int16_t, 2> expected{-1, -1};
    std::array<std::array<std::int8_t, S>, 2> pending{};
    std::array<std::uint8_t, 2> pending_count{0, 0};
    std::array<std::int8_t, 2> heading{-1, -1};
    std::array<std::uint8_t, 2> escape_steps{0, 0};
};

EscapeMemory g_escape;

GameOutput safeOutput() {
    return GameOutput{{kStay, kStay, kStay, kStay, kStay, kStay}, 3, 0, 0};
}

inline bool inside(int r, int c) {
    return static_cast<unsigned>(r) < GRID_SIZE &&
           static_cast<unsigned>(c) < GRID_SIZE;
}
inline int ix(int r, int c) { return r * GRID_SIZE + c; }
inline int dist(int ar, int ac, int br, int bc) {
    const int dr = ar > br ? ar - br : br - ar;
    const int dc = ac > bc ? ac - bc : bc - ac;
    return dr + dc;
}

struct Target {
    int score = std::numeric_limits<int>::min();
    std::int16_t cell = -1;
};

struct Context {
    std::array<std::uint8_t, kCells> actors{};
    std::array<std::uint8_t, kCells> reach{};
    std::array<Target, 2> targets{};
    int visible_gold = 0;
};

void insertTarget(Target& slot, Target target) {
    if (target.cell < 0) return;
    if (slot.cell == target.cell) {
        if (target.score > slot.score) slot = target;
        return;
    }
    if (target.score > slot.score) slot = target;
}

int repeatedPickup(int amount, int visits) {
    int total = 0;
    while (visits-- > 0 && amount > 0) {
        const int got = (65 * amount + 99) / 100;
        amount -= got;
        total += got;
    }
    return total;
}

void buildContext(const GameInput& in, Context& x) {
    constexpr std::array<std::array<int, 2>, 2> patrol{{{6, 8}, {10, 8}}};
    for (int unit = 0; unit < 2; ++unit) {
        const int cell = ix(patrol[unit][0], patrol[unit][1]);
        x.targets[unit] = Target{0, static_cast<std::int16_t>(cell)};
    }
    for (const Position& p : in.visible_enemies) {
        if (inside(p.row, p.col)) {
            x.actors[ix(p.row, p.col)] |= 0x80;
        }
    }
    const int npc_count = std::clamp(in.num_visible_npcs, 0, MAX_NPCS);
    for (int i = 0; i < npc_count; ++i) {
        const Position p = in.visible_npcs[i].pos;
        if (!inside(p.row, p.col)) continue;
        ++x.actors[ix(p.row, p.col)];
        for (int dr = -3; dr <= 3; ++dr) {
            const int row = p.row + dr;
            if (static_cast<unsigned>(row) >= GRID_SIZE) continue;
            const int span = 3 - (dr < 0 ? -dr : dr);
            for (int dc = -span; dc <= span; ++dc) {
                const int col = p.col + dc;
                if (static_cast<unsigned>(col) < GRID_SIZE) {
                    ++x.reach[ix(row, col)];
                }
            }
        }
    }
    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE; ++c) {
            const int gold = in.grid[r][c];
            if (gold <= 0) continue;
            x.visible_gold += gold;
            for (int unit = 0; unit < 2; ++unit) {
                const int d = dist(
                    in.my_units[unit].row,
                    in.my_units[unit].col,
                    r,
                    c);
                constexpr int budget = S;
                int visits = 0;
                if (d == 0) {
                    visits = budget / 2;
                } else if (d <= budget) {
                    visits = 1 + (budget - d) / 2;
                }
                int score;
                int collected = 0;
                if (visits > 0) {
                    collected = repeatedPickup(gold, visits);
                    score = 240 * collected - 19 * d;
                } else {
                    const int first = (65 * gold + 99) / 100;
                    score =
                        55 * first * budget / std::max(1, d) - 19 * budget;
                }
                if (x.reach[ix(r, c)] >= 3 &&
                    in.my_units_gold[unit] + collected >= 100) {
                    score -= 12000;
                }
                insertTarget(
                    x.targets[unit],
                    Target{score, static_cast<std::int16_t>(ix(r, c))});
            }
        }
    }
}

struct GoldSeen {
    int cell = -1;
    int remaining = 0;
};

int pickupAt(
    const GameInput& in,
    int cell,
    std::array<GoldSeen, S>& seen,
    int& count) {
    if (in.grid[cell / GRID_SIZE][cell % GRID_SIZE] <= 0) return 0;
    int slot = -1;
    for (int i = 0; i < count; ++i) {
        if (seen[i].cell == cell) slot = i;
    }
    if (slot < 0) {
        slot = count++;
        seen[slot] = GoldSeen{cell, in.grid[cell / GRID_SIZE][cell % GRID_SIZE]};
    }
    if (seen[slot].remaining <= 0) return 0;
    const int got = (65 * seen[slot].remaining + 99) / 100;
    seen[slot].remaining -= got;
    return got;
}

struct Route {
    std::uint32_t actions = 0;
    std::array<int, S + 1> prefix_score{};
};

inline int actionAt(const Route& route, int step) {
    return static_cast<int>((route.actions >> (3 * step)) & 7U);
}
inline void setAction(Route& route, int step, int action) {
    route.actions |= static_cast<std::uint32_t>(action) << (3 * step);
}

Route plan(
    const GameInput& in,
    const Context& x,
    int unit,
    int budget,
    int target_cell) {
    Route route;
    route.actions = 0;
    int r = in.my_units[unit].row;
    int c = in.my_units[unit].col;
    const int target_r = target_cell / GRID_SIZE;
    const int target_c = target_cell % GRID_SIZE;
    int held = std::max(0, in.my_units_gold[unit]);
    int raw_score = 0;
    std::array<GoldSeen, S> seen{};
    int seen_count = 0;

    for (int step = 0; step < budget; ++step) {
        if (r == target_r && c == target_c && in.grid[r][c] > 0 &&
            step + 2 <= budget) {
            int bounce = -1;
            int bounce_risk = std::numeric_limits<int>::max();
            for (int action = 0; action < 4; ++action) {
                const int nr = r + kDr[action], nc = c + kDc[action];
                if (!inside(nr, nc) || in.grid[nr][nc] < 0) continue;
                const auto actor = x.actors[ix(nr, nc)];
                if ((actor & 0x80U) || (actor & 0x7fU) >= 3) continue;
                const int risk = 4 * x.reach[ix(nr, nc)] +
                                 (dist(nr, nc, 8, 8) <= 2);
                if (risk < bounce_risk) {
                    bounce_risk = risk;
                    bounce = action;
                }
            }
            if (bounce >= 0) {
                setAction(route, step, bounce);
                raw_score += 2;
                route.prefix_score[step + 1] = raw_score;
                ++step;
                setAction(route, step, kReverse[bounce]);
                const int got = pickupAt(in, ix(r, c), seen, seen_count);
                held += got;
                raw_score += 240 * got + 2;
                route.prefix_score[step + 1] = raw_score;
                continue;
            }
        }

        int best_action = kStay;
        int best_score = std::numeric_limits<int>::min();
        for (int action = 0; action <= kStay; ++action) {
            const int nr = r + kDr[action], nc = c + kDc[action];
            if (!inside(nr, nc) ||
                (action != kStay && in.grid[nr][nc] < 0)) {
                continue;
            }
            const auto actor = x.actors[ix(nr, nc)];
            if ((actor & 0x80U) || (actor & 0x7fU) >= 3) continue;
            int score = action == kStay ? 0 : 3;
            int amount = 0;
            if (action != kStay && in.grid[nr][nc] > 0) {
                int remaining = in.grid[nr][nc];
                for (int i = 0; i < seen_count; ++i) {
                    if (seen[i].cell == ix(nr, nc)) {
                        remaining = seen[i].remaining;
                    }
                }
                amount = remaining > 0 ? (65 * remaining + 99) / 100 : 0;
                score += 240 * amount;
            }
            score += 50 * (dist(r, c, target_r, target_c) -
                           dist(nr, nc, target_r, target_c));
            const int patrol_r = unit == 0 ? 6 : 10;
            score += 25 * (dist(r, c, patrol_r, 8) -
                           dist(nr, nc, patrol_r, 8));
            if (x.reach[ix(nr, nc)] >= 3 && held + amount >= 100) continue;
            if (x.reach[ix(nr, nc)] >= 3) {
                score -= 45 * ((5 * (held + amount) + 99) / 100);
            }
            if (score > best_score) {
                best_score = score;
                best_action = action;
            }
        }
        setAction(route, step, best_action);
        if (best_action != kStay) {
            r += kDr[best_action];
            c += kDc[best_action];
            const int got = pickupAt(in, ix(r, c), seen, seen_count);
            held += got;
            raw_score += 240 * got + 2;
            if (x.reach[ix(r, c)] >= 3) {
                raw_score -= 45 * ((5 * held + 99) / 100);
            }
        }
        route.prefix_score[step + 1] = raw_score;
    }
    return route;
}

struct SimGold {
    int cell = -1;
    int remaining = 0;
};

int exactScore(
    const GameInput& in,
    const Context& x,
    GameOutput& output) {
    std::array<int, 2> pos{
        ix(in.my_units[0].row, in.my_units[0].col),
        ix(in.my_units[1].row, in.my_units[1].col),
    };
    std::array<int, 2> held{
        std::max(0, in.my_units_gold[0]), std::max(0, in.my_units_gold[1]),
    };
    std::array<SimGold, S> seen{};
    int seen_count = 0;
    int score = -5 * (output.k > 3 ? output.k - 3 : 3 - output.k);
    for (int sequence = 0; sequence < 2; ++sequence) {
        const int unit = sequence == 0 ? output.order : 1 - output.order;
        const int other = 1 - unit;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int slot = begin; slot < end; ++slot) {
            const int action = output.actions[slot];
            if (action == kStay) continue;
            const int row = pos[unit] / GRID_SIZE + kDr[action];
            const int col = pos[unit] % GRID_SIZE + kDc[action];
            if (!inside(row, col)) {
                output.actions[slot] = kStay;
                continue;
            }
            const int next = ix(row, col);
            const auto actor = x.actors[next];
            if (in.grid[row][col] < 0 || (actor & 0x80U) ||
                (actor & 0x7fU) >= 3 || next == pos[other]) {
                output.actions[slot] = kStay;
                continue;
            }
            pos[unit] = next;
            score += 2;
            if (in.grid[row][col] > 0) {
                int gi = -1;
                for (int i = 0; i < seen_count; ++i) {
                    if (seen[i].cell == next) gi = i;
                }
                if (gi < 0) {
                    gi = seen_count++;
                    seen[gi] = SimGold{next, in.grid[row][col]};
                }
                const int got = (65 * seen[gi].remaining + 99) / 100;
                seen[gi].remaining -= got;
                held[unit] += got;
                score += 240 * got;
            }
            if (x.reach[next] >= 3) {
                if (held[unit] >= 100) score -= 12000;
                score -= 45 * ((5 * held[unit] + 99) / 100);
            }
        }
    }
    return score;
}

struct SafetyState {
    std::array<std::uint16_t, 2> pos{};
};

void enforceSafety(const GameInput& in, const Context& x, GameOutput& output) {
restart:
    // A prior stall-safety deletion changes the nominal suffix origin.  Reuse
    // the compact exact simulator to remove any newly exposed wall, hazard or
    // occupied-cell action before expanding arbitrary success/stall states.
    (void)exactScore(in, x, output);
    // Every state is written before it is read; zeroing all 64 entries only
    // adds hot-path work and does not contribute to the closure semantics.
    std::array<SafetyState, 1 << S> states;
    states[0].pos = {
        static_cast<std::uint16_t>(
            ix(in.my_units[0].row, in.my_units[0].col)),
        static_cast<std::uint16_t>(
            ix(in.my_units[1].row, in.my_units[1].col)),
    };
    int state_count = 1;
    for (int sequence = 0; sequence < 2; ++sequence) {
        const int unit = sequence == 0 ? output.order : 1 - output.order;
        const int other = 1 - unit;
        const int begin = unit == 0 ? 0 : output.k;
        const int end = unit == 0 ? output.k : S;
        for (int slot = begin; slot < end; ++slot) {
            const int action = output.actions[slot];
            if (action == kStay) continue;
            const int before = state_count;
            for (int state = 0; state < before; ++state) {
                const int old = states[state].pos[unit];
                const int sr = old / GRID_SIZE + kDr[action];
                const int sc = old % GRID_SIZE + kDc[action];
                if (!inside(sr, sc)) {
                    output.actions[slot] = kStay;
                    goto restart;
                }
                const int next = ix(sr, sc);
                const int cell = in.grid[sr][sc];
                const std::uint8_t actor = x.actors[next];
                const bool forced_stall =
                    cell == -1 || next == states[state].pos[other];
                if (forced_stall) continue;
                if (cell < 0 || (actor & 0x7fU) >= 3) {
                    output.actions[slot] = kStay;
                    goto restart;
                }

                // Every currently safe action may either succeed or be
                // occupied before our turn.  Preserve the failure state and
                // append its success twin.  Six actions give exactly 64
                // states, so the closure has a fixed hard upper bound.
                states[state_count] = states[state];
                states[state_count].pos[unit] =
                    static_cast<std::uint16_t>(next);
                ++state_count;
            }
        }
    }
}

bool cornerEscapeRoute(
    const GameInput& in,
    const Context& x,
    int unit,
    std::array<int, 3>& route) {
    const Position start = in.my_units[unit];
    if (start.row == 2 && start.col == 0) {
        route = {0, 3, 3};  // up, right, right
    } else if (start.row == 2 && start.col == 16) {
        route = {0, 2, 2};  // up, left, left
    } else if (start.row == 14 && start.col == 0) {
        route = {1, 3, 3};  // down, right, right
    } else if (start.row == 14 && start.col == 16) {
        route = {1, 2, 2};  // down, left, left
    } else {
        return false;
    }

    int row = start.row;
    int col = start.col;
    const int other = ix(
        in.my_units[1 - unit].row,
        in.my_units[1 - unit].col);
    for (int action : route) {
        row += kDr[action];
        col += kDc[action];
        if (!inside(row, col) || in.grid[row][col] < 0) return false;
        const int cell = ix(row, col);
        const std::uint8_t actor = x.actors[cell];
        if ((actor & 0x80U) || (actor & 0x7fU) >= 3 ||
            x.reach[cell] >= 3 || cell == other) {
            return false;
        }
    }
    return true;
}

bool unitAllStay(const GameOutput& output, int unit) {
    const int begin = unit == 0 ? 0 : output.k;
    const int end = unit == 0 ? output.k : S;
    for (int slot = begin; slot < end; ++slot) {
        if (output.actions[slot] != kStay) return false;
    }
    return true;
}

int straightEscapeLength(
    const GameInput& in,
    const Context& x,
    int unit,
    int direction,
    int limit,
    std::array<int, S>& route) {
    int row = in.my_units[unit].row;
    int col = in.my_units[unit].col;
    const int other = ix(
        in.my_units[1 - unit].row,
        in.my_units[1 - unit].col);
    int length = 0;
    while (length < limit) {
        row += kDr[direction];
        col += kDc[direction];
        if (!inside(row, col) || in.grid[row][col] < 0) break;
        const int cell = ix(row, col);
        const std::uint8_t actor = x.actors[cell];
        if ((actor & 0x80U) || (actor & 0x7fU) >= 3 ||
            x.reach[cell] >= 3 || cell == other) {
            break;
        }
        route[length++] = direction;
    }
    return length;
}

int visibleOpenDegree(
    const GameInput& in,
    const Context& x,
    int unit,
    int row,
    int col) {
    const int other = ix(
        in.my_units[1 - unit].row,
        in.my_units[1 - unit].col);
    int degree = 0;
    for (int action = 0; action < 4; ++action) {
        const int nr = row + kDr[action];
        const int nc = col + kDc[action];
        if (!inside(nr, nc) || in.grid[nr][nc] < 0) continue;
        const int cell = ix(nr, nc);
        const std::uint8_t actor = x.actors[cell];
        degree += !((actor & 0x80U) || (actor & 0x7fU) >= 3 ||
                    x.reach[cell] >= 3 ||
                    cell == other);
    }
    return degree;
}

int bestEscapeHeading(
    const GameInput& in,
    const Context& x,
    int unit,
    std::array<int, S>& route,
    int& length) {
    int best_direction = -1;
    int best_score = -1;
    for (int direction = 0; direction < 4; ++direction) {
        std::array<int, S> candidate{};
        const int candidate_length =
            straightEscapeLength(in, x, unit, direction, S, candidate);
        if (candidate_length == 0) continue;
        const int row = in.my_units[unit].row +
            candidate_length * kDr[direction];
        const int col = in.my_units[unit].col +
            candidate_length * kDc[direction];
        const int score = 16 * candidate_length +
            4 * visibleOpenDegree(in, x, unit, row, col) - direction;
        if (score > best_score) {
            best_score = score;
            best_direction = direction;
            length = candidate_length;
            route = candidate;
        }
    }
    return best_direction;
}

void applyStallEscape(
    const GameInput& in,
    const Context& x,
    GameOutput& output) {
    const bool consecutive = g_escape.last_round + 1 == in.round;
    std::array<std::int16_t, 2> current{};
    std::array<std::uint8_t, 2> completed_stalls{};
    for (int unit = 0; unit < 2; ++unit) {
        current[unit] = static_cast<std::int16_t>(ix(
            in.my_units[unit].row,
            in.my_units[unit].col));
        if (consecutive && g_escape.final_allstay[unit] != 0 &&
            g_escape.last_position[unit] == current[unit]) {
            completed_stalls[unit] = static_cast<std::uint8_t>(std::min(
                3,
                static_cast<int>(g_escape.stall_transitions[unit]) + 1));
        }
    }

    std::array<bool, 2> active{false, false};
    std::array<bool, 2> continuation{false, false};
    std::array<int, 2> headings{-1, -1};
    std::array<int, 2> lengths{0, 0};
    std::array<std::array<int, S>, 2> routes{};
    for (int unit = 0; unit < 2; ++unit) {
        const int budget = unit == 0 ? output.k : S - output.k;
        const bool can_borrow_one = budget == 0;
        continuation[unit] = consecutive &&
            g_escape.expected[unit] == current[unit] &&
            g_escape.heading[unit] >= 0 &&
            g_escape.escape_steps[unit] < S && x.visible_gold < 8;
        if ((budget <= 0 && !can_borrow_one) ||
            (!continuation[unit] && !unitAllStay(output, unit))) {
            continue;
        }

        if (continuation[unit]) {
            if (g_escape.pending_count[unit] > 0) {
                lengths[unit] = g_escape.pending_count[unit];
                for (int step = 0; step < lengths[unit]; ++step) {
                    routes[unit][step] = g_escape.pending[unit][step];
                }
            } else {
                lengths[unit] = 1;
                routes[unit][0] = g_escape.heading[unit];
            }
            headings[unit] = routes[unit][0];
            std::array<int, S> one_step{};
            if (straightEscapeLength(
                    in, x, unit, headings[unit], 1, one_step) != 1) {
                continue;
            }
        } else {
            const int required_stalls = x.visible_gold == 0 ? 1 : 2;
            if (x.visible_gold >= 8 ||
                completed_stalls[unit] < required_stalls) {
                continue;
            }
            std::array<int, 3> corner{};
            if (cornerEscapeRoute(in, x, unit, corner)) {
                for (int step = 0; step < 3; ++step) {
                    routes[unit][step] = corner[step];
                }
                headings[unit] = corner[0];
                lengths[unit] = 3;
            } else {
                headings[unit] = bestEscapeHeading(
                    in, x, unit, routes[unit], lengths[unit]);
            }
        }
        active[unit] = headings[unit] >= 0 && lengths[unit] > 0;
    }

    if (active[0] || active[1]) {
        if (active[0] && output.k == 0) {
            // Give unit 0 one slot while preserving the productive prefix of
            // unit 1; only the far tail can be displaced.
            for (int slot = S - 1; slot > 0; --slot) {
                output.actions[slot] = output.actions[slot - 1];
            }
            output.k = 1;
        } else if (active[1] && output.k == S) {
            // Symmetrically retain the first five unit-0 actions.
            output.k = S - 1;
        }

        for (int unit = 0; unit < 2; ++unit) {
            if (!active[unit]) continue;
            const int begin = unit == 0 ? 0 : output.k;
            const int end = unit == 0 ? output.k : S;
            // A continuation is a one-step commitment: keeping this unit's
            // empty-space suffix can immediately walk back into the state we
            // just escaped.  The other unit's route remains intact.
            if (continuation[unit]) {
                for (int slot = begin; slot < end; ++slot) {
                    output.actions[slot] = kStay;
                }
            }
            output.actions[begin] = headings[unit];
        }
        enforceSafety(in, x, output);
    }

    EscapeMemory next;
    next.last_round = in.round;
    next.last_position = current;
    next.stall_transitions = completed_stalls;
    for (int unit = 0; unit < 2; ++unit) {
        next.final_allstay[unit] = unitAllStay(output, unit);
    }
    for (int unit = 0; unit < 2; ++unit) {
        if (!active[unit]) continue;
        const int slot = unit == 0 ? 0 : output.k;
        if (output.actions[slot] == kStay) continue;
        const int row = in.my_units[unit].row + kDr[headings[unit]];
        const int col = in.my_units[unit].col + kDc[headings[unit]];
        next.expected[unit] = static_cast<std::int16_t>(ix(row, col));
        next.heading[unit] = static_cast<std::int8_t>(headings[unit]);
        next.escape_steps[unit] = static_cast<std::uint8_t>(
            continuation[unit] ? g_escape.escape_steps[unit] + 1 : 1);
        next.pending_count[unit] = static_cast<std::uint8_t>(
            lengths[unit] - 1);
        for (int step = 1; step < lengths[unit]; ++step) {
            next.pending[unit][step - 1] =
                static_cast<std::int8_t>(routes[unit][step]);
        }
    }
    g_escape = next;
}

GameOutput decide(const GameInput& in) {
    Context x;
    buildContext(in, x);
    const Route route0 = plan(in, x, 0, S, x.targets[0].cell);
    const Route route1 = plan(in, x, 1, S, x.targets[1].cell);

    int split = 0;
    int raw = route0.prefix_score[0] + route1.prefix_score[S] - 15;
    for (int candidate = 1; candidate <= S; ++candidate) {
        const int candidate_raw = route0.prefix_score[candidate] +
            route1.prefix_score[S - candidate] -
            5 * (candidate > 3 ? candidate - 3 : 3 - candidate);
        if (candidate_raw > raw) {
            raw = candidate_raw;
            split = candidate;
        }
    }

    GameOutput best{{4, 4, 4, 4, 4, 4}, split, 0, 0};
    for (int i = 0; i < split; ++i) {
        best.actions[i] = actionAt(route0, i);
    }
    for (int i = split; i < S; ++i) {
        best.actions[i] = actionAt(route1, i - split);
    }

    // The former one-element candidate loop called exactScore here and then
    // enforceSafety called it again.  Only the latter result can escape this
    // function, so the first simulation was strictly redundant.
    enforceSafety(in, x, best);
    applyStallEscape(in, x, best);
    if (in.round >= 15 && in.round < 470 && in.round - g_last_vp >= 20 &&
        in.my_units_gold[0] + in.my_units_gold[1] >= 20 &&
        x.visible_gold == 0) {
        best.vp = 1;
        g_last_vp = in.round;
    }
    return best;
}

// This strategy has one deliberately bounded path.  Keep the complete path
// ABI so later variants can add slow or refresh rounds without changing the
// benchmark and test harnesses.
#if defined(GOLD_RUSH_BENCHMARKING) || defined(GOLD_RUSH_TESTING)
void resetStats() {
    g_rounds = 0;
    g_stats = true;
}

void stopStats() {
    g_stats = false;
}
#endif

}  // namespace

extern "C" __attribute__((visibility("default")))
GameOutput moveDecision(const GameInput* input) {
    if (input == nullptr) return safeOutput();
    if (input->round == 0) {
        g_last_vp = -1000;
        g_escape = EscapeMemory{};
    }
#if defined(GOLD_RUSH_BENCHMARKING) || defined(GOLD_RUSH_TESTING)
    if (g_stats) ++g_rounds;
#endif
    try {
        return decide(*input);
    } catch (...) {
        return safeOutput();
    }
}

#ifdef GOLD_RUSH_BENCHMARKING
extern "C" void goldrush_sub1_benchmark_reset_stats() { resetStats(); }
extern "C" void goldrush_sub1_benchmark_stop_stats() { stopStats(); }
extern "C" std::uint64_t goldrush_sub1_benchmark_rounds() {
    return g_rounds;
}
extern "C" std::uint64_t goldrush_sub1_benchmark_fast_rounds() {
    return g_rounds;
}
extern "C" std::uint64_t goldrush_sub1_benchmark_slow_rounds() { return 0; }
extern "C" std::uint64_t goldrush_sub1_benchmark_refresh_rounds() {
    return 0;
}
extern "C" int goldrush_sub1_benchmark_last_path() { return 0; }
#endif

#ifdef GOLD_RUSH_TESTING
extern "C" void goldrush_sub1_test_reset_stats() { resetStats(); }
extern "C" void goldrush_sub1_test_stop_stats() { stopStats(); }
extern "C" std::uint64_t goldrush_sub1_test_rounds() { return g_rounds; }
extern "C" std::uint64_t goldrush_sub1_test_fast_rounds() {
    return g_rounds;
}
extern "C" std::uint64_t goldrush_sub1_test_slow_rounds() { return 0; }
extern "C" std::uint64_t goldrush_sub1_test_refresh_rounds() { return 0; }
extern "C" int goldrush_sub1_test_last_path() { return 0; }
extern "C" GameOutput goldrush_sub1_test_enforce_safety(
    const GameInput* input,
    GameOutput proposed) {
    if (input == nullptr) return safeOutput();
    try {
        Context context;
        buildContext(*input, context);
        enforceSafety(*input, context, proposed);
        return proposed;
    } catch (...) {
        return safeOutput();
    }
}
extern "C" GameOutput goldrush_awa19i_test_apply_stall_escape(
    const GameInput* input,
    GameOutput proposed) {
    if (input == nullptr) return safeOutput();
    if (input->round == 0) g_escape = EscapeMemory{};
    try {
        Context context;
        buildContext(*input, context);
        applyStallEscape(*input, context, proposed);
        return proposed;
    } catch (...) {
        return safeOutput();
    }
}
#endif
