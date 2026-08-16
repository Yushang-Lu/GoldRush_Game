#!/usr/bin/env python3
"""Per-match report and counterfactual audit for GoldRush JSON-lines logs."""

from __future__ import annotations

import argparse
import collections
import ctypes
import pathlib
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOLS = ROOT / "code" / "tools"
sys.path.insert(0, str(TOOLS))
from log_analyzer import MatchLog, percentile, player, read_log  # noqa: E402
from replay_compare import (  # noqa: E402
    GameInput,
    NpcInfo,
    Position,
    RegionStat,
    Strategy,
    evaluate,
)


def build_input(turn: dict, own_id: int) -> GameInput:
    result = GameInput()
    result.round = turn["round"]
    start = turn["start"]
    own = player(start, own_id)
    opponent = player(start, 3 - own_id)
    for row, values in enumerate(start["grid"]):
        for col, value in enumerate(values):
            result.grid[row][col] = value
    for unit, value in enumerate(own["units"]):
        result.my_units[unit] = Position(*value["position"])
        result.my_units_gold[unit] = value["gold"]
    result.gold_opp = opponent["gold"]
    enemies = [
        unit["position"]
        for unit in opponent["units"]
        if unit.get("position") is not None
    ]
    for index in range(2):
        result.visible_enemies[index] = Position(
            *(enemies[index] if index < len(enemies) else (-1, -1))
        )
    npcs = start.get("npcs", [])[:7]
    result.num_visible_npcs = len(npcs)
    for index in range(7):
        result.visible_npcs[index] = (
            NpcInfo(npcs[index]["id"], Position(*npcs[index]["position"]))
            if index < len(npcs)
            else NpcInfo(0, Position(-1, -1))
        )
    snapshot = turn.get("snapshot")
    result.snapshot_valid = int(snapshot is not None)
    result.snapshot.window_begin = -1
    result.snapshot.window_end = -1
    if snapshot:
        result.snapshot.window_begin, result.snapshot.window_end = snapshot["window"]
        for index, stat in enumerate(snapshot["regions"]):
            result.snapshot.regions[index] = RegionStat(
                *(stat[field] for field, _ in RegionStat._fields_)
            )
    return result


def region(position: tuple[int, int]) -> int:
    row, col = position
    if 4 <= row <= 12 and 4 <= col <= 12:
        return 1
    if row <= 3 and col <= 12:
        return 2
    if row >= 4 and col <= 3:
        return 3
    if row >= 13 and col >= 4:
        return 4
    return 5


@dataclass
class Actual:
    opponent: str
    own_net: int
    opponent_net: int
    first: int
    p90: float
    pickup: int
    inferred_opponent_pickup: int
    loss: int
    unit_pickup: tuple[int, int]
    active_moves: int
    core_fraction: float
    marker_rounds: int


def actual(match: MatchLog, own_id: int) -> Actual:
    opponent_id = 3 - own_id
    own_name = match.players[f"player{own_id}"]
    opponent_name = match.players[f"player{opponent_id}"]
    del own_name
    costs: list[int] = []
    first = pickup = loss = active = marker_rounds = core_rounds = 0
    unit_pickup = [0, 0]
    position_samples = 0
    for turn in match.turns:
        start = player(turn["start"], own_id)
        end = player(turn["end"], own_id)
        costs.append(end.get("cost", 0))
        dispatch = turn["end"].get("dispatch_order", [])
        first += (
            own_id in dispatch
            and opponent_id in dispatch
            and dispatch.index(own_id) < dispatch.index(opponent_id)
        )
        for unit in range(2):
            amount = end["units"][unit].get("pickup", 0)
            pickup += amount
            unit_pickup[unit] += amount
            loss += (
                start["units"][unit]["gold"]
                + amount
                - end["units"][unit]["gold"]
            )
            actions = end["units"][unit].get("actions", [])
            active += sum(action != 4 for action in actions)
            position = tuple(end["units"][unit]["position"])
            position_samples += 1
            core_rounds += region(position) == 1
            marker_rounds += match.board[position[0]][position[1]] == "2"
    final_own = player(match.turns[-1]["end"], own_id)
    final_opponent = player(match.turns[-1]["end"], opponent_id)
    own_net = final_own["gold"] - final_own.get("vision_spent", 0)
    opponent_net = final_opponent["gold"] - final_opponent.get("vision_spent", 0)
    burned = sum(turn["end"].get("burned", 0) for turn in match.turns)
    opponent_loss = max(0, burned - loss)
    return Actual(
        opponent_name,
        own_net,
        opponent_net,
        first,
        percentile(costs, 0.90),
        pickup,
        final_opponent["gold"] + opponent_loss,
        loss,
        (unit_pickup[0], unit_pickup[1]),
        active,
        core_rounds / max(1, position_samples),
        marker_rounds,
    )


def audit(match: MatchLog, own_id: int, strategy: Strategy) -> tuple[int, ...]:
    values = [0] * 6
    for turn in match.turns:
        game = build_input(turn, own_id)
        output = strategy.decide(ctypes.byref(game))
        if (
            any(action < 0 or action > 4 for action in output.actions)
            or output.k < 0
            or output.k > 6
            or output.order not in (0, 1)
            or output.vp not in (0, 1, 2)
        ):
            raise ValueError(f"{strategy.path}: illegal output at round {turn['round']}")
        result = evaluate(game, output)
        current = (
            result.pickup,
            result.known_loss,
            result.crowded_loss,
            result.fog_moves,
            result.blocked,
            result.moves,
        )
        values = [left + right for left, right in zip(values, current)]
    return tuple(values)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=pathlib.Path)
    parser.add_argument("--own-id", type=int, choices=(1, 2), default=2)
    parser.add_argument("--strategy", action="append", type=pathlib.Path, default=[])
    args = parser.parse_args(argv)
    matches = [read_log(path) for path in args.logs]
    strategies = [Strategy(path) for path in args.strategy]

    print(
        "| log | opponent | score own:opp | first | P90 us | "
        "pickup own:opp* | loss | unit pickup | active | core | mine rounds |"
    )
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for match in matches:
        value = actual(match, args.own_id)
        print(
            f"| {match.path.stem.removeprefix('game_')} | {value.opponent} | "
            f"{value.own_net}:{value.opponent_net} | {value.first}/500 | "
            f"{value.p90:.3f} | {value.pickup}:{value.inferred_opponent_pickup} | "
            f"{value.loss} | {value.unit_pickup[0]}/{value.unit_pickup[1]} | "
            f"{value.active_moves} | {value.core_fraction:.1%} | "
            f"{value.marker_rounds} |"
        )
    print("\n* opponent pickup is inferred as final gross gold plus non-own burned gold.\n")

    for strategy in strategies:
        aggregate = [0] * 6
        print(f"strategy={strategy.path}")
        for match in matches:
            values = audit(match, args.own_id, strategy)
            aggregate = [left + right for left, right in zip(aggregate, values)]
            print(
                f"  {match.path.stem}: pickup={values[0]} bomb={values[1]} "
                f"crowd={values[2]} fog={values[3]} blocked={values[4]} "
                f"moves={values[5]}"
            )
        print(
            "  aggregate: "
            f"pickup={aggregate[0]} bomb={aggregate[1]} crowd={aggregate[2]} "
            f"fog={aggregate[3]} blocked={aggregate[4]} moves={aggregate[5]}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
