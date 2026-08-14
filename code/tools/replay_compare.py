#!/usr/bin/env python3
"""Replay logged P1 inputs through two strategy libraries and compare choices.

This is a counterfactual decision audit, not a simulator: hidden opponent/NPC
movement is unknowable, so it reports only deterministic value available in
the decision input (visible/remembered movement, pickup and known hazards).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Any

from log_analyzer import MatchLog, player, read_log

GRID_SIZE = 17
MAX_NPCS = 7
REGION_COUNT = 5
STEPS = 6
MOVES = ((-1, 0), (1, 0), (0, -1), (0, 1), (0, 0))


class Position(ctypes.Structure):
    _fields_ = (("row", ctypes.c_int), ("col", ctypes.c_int))


class NpcInfo(ctypes.Structure):
    _fields_ = (("id", ctypes.c_int), ("pos", Position))


class RegionStat(ctypes.Structure):
    _fields_ = tuple(
        (name, ctypes.c_int)
        for name in (
            "id",
            "enter",
            "leave",
            "gold_generated",
            "gold_collected",
            "gold_remaining",
            "occupants",
        )
    )


class Snapshot(ctypes.Structure):
    _fields_ = (
        ("window_begin", ctypes.c_int),
        ("window_end", ctypes.c_int),
        ("regions", RegionStat * REGION_COUNT),
    )


class GameInput(ctypes.Structure):
    _fields_ = (
        ("round", ctypes.c_int),
        ("grid", (ctypes.c_int * GRID_SIZE) * GRID_SIZE),
        ("my_units", Position * 2),
        ("my_units_gold", ctypes.c_int * 2),
        ("gold_opp", ctypes.c_int),
        ("visible_enemies", Position * 2),
        ("num_visible_npcs", ctypes.c_int),
        ("visible_npcs", NpcInfo * MAX_NPCS),
        ("snapshot_valid", ctypes.c_int),
        ("snapshot", Snapshot),
    )


class GameOutput(ctypes.Structure):
    _fields_ = (
        ("actions", ctypes.c_int * STEPS),
        ("k", ctypes.c_int),
        ("order", ctypes.c_int),
        ("vp", ctypes.c_int),
    )


class Strategy:
    def __init__(self, path: pathlib.Path) -> None:
        self.path = path.resolve()
        self.library = ctypes.CDLL(str(self.path))
        self.decide = self.library.moveDecision
        self.decide.argtypes = (ctypes.POINTER(GameInput),)
        self.decide.restype = GameOutput


@dataclass(frozen=True)
class Evaluation:
    pickup: int
    known_loss: int
    remembered_bomb_loss: int
    remembered_bomb_entries: int
    fog_moves: int
    blocked: int


def build_input(turn: dict[str, Any]) -> GameInput:
    result = GameInput()
    result.round = turn["round"]
    start = turn["start"]
    own = player(start, 1)
    opponent = player(start, 2)
    for row, values in enumerate(start["grid"]):
        for col, value in enumerate(values):
            result.grid[row][col] = value
    for unit, value in enumerate(own["units"]):
        result.my_units[unit] = Position(*value["position"])
        result.my_units_gold[unit] = value["gold"]
    result.gold_opp = opponent["gold"]
    enemies = [unit["position"] for unit in opponent["units"] if unit.get("position")]
    for index in range(2):
        result.visible_enemies[index] = Position(
            *(enemies[index] if index < len(enemies) else (-1, -1))
        )
    npcs = start.get("npcs", [])[:MAX_NPCS]
    result.num_visible_npcs = len(npcs)
    for index in range(MAX_NPCS):
        if index < len(npcs):
            result.visible_npcs[index] = NpcInfo(
                npcs[index]["id"], Position(*npcs[index]["position"])
            )
        else:
            result.visible_npcs[index] = NpcInfo(0, Position(-1, -1))
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


def evaluate(
    game: GameInput,
    output: GameOutput,
    remembered_bombs: set[tuple[int, int]] | None = None,
) -> Evaluation:
    positions = [[game.my_units[i].row, game.my_units[i].col] for i in range(2)]
    held = [int(game.my_units_gold[i]) for i in range(2)]
    remaining: dict[tuple[int, int], int] = {}
    consumed_bombs: set[tuple[int, int]] = set()
    pickup = known_loss = remembered_bomb_loss = remembered_bomb_entries = 0
    fog_moves = blocked = 0
    remembered_bombs = remembered_bombs or set()
    for unit in (output.order, 1 - output.order):
        begin, end = (0, output.k) if unit == 0 else (output.k, STEPS)
        for action_index in range(begin, end):
            action = output.actions[action_index]
            if action == 4:
                continue
            dr, dc = MOVES[action]
            row = positions[unit][0] + dr
            col = positions[unit][1] + dc
            if not (0 <= row < GRID_SIZE and 0 <= col < GRID_SIZE):
                blocked += 1
                continue
            value = game.grid[row][col]
            if value == -1 or [row, col] == positions[1 - unit]:
                blocked += 1
                continue
            positions[unit] = [row, col]
            cell = (row, col)
            if value == -5:
                fog_moves += 1
                if cell in remembered_bombs and cell not in consumed_bombs:
                    loss = (held[unit] + 9) // 10
                    held[unit] -= loss
                    remembered_bomb_loss += loss
                    remembered_bomb_entries += 1
                    consumed_bombs.add(cell)
                continue
            if value > 0:
                left = remaining.get(cell, value)
                amount = (left * 65 + 99) // 100
                pickup += amount
                held[unit] += amount
                remaining[cell] = left - amount
            if value == -3 and cell not in consumed_bombs:
                loss = (held[unit] + 9) // 10
                held[unit] -= loss
                known_loss += loss
                consumed_bombs.add(cell)
    return Evaluation(
        pickup,
        known_loss,
        remembered_bomb_loss,
        remembered_bomb_entries,
        fog_moves,
        blocked,
    )


def late_hand_input(game: GameInput) -> GameInput:
    """Apply the one-pickup contest model used after slow-hand evidence.

    This is deliberately only a visible-information heuristic.  It discounts
    each positive tile at or next to a visible enemy/NPC once; it does not
    claim to reconstruct hidden movement.
    """
    result = GameInput()
    ctypes.memmove(ctypes.byref(result), ctypes.byref(game), ctypes.sizeof(game))
    marked: set[tuple[int, int]] = set()
    own = {(game.my_units[i].row, game.my_units[i].col) for i in range(2)}
    offsets = ((0, 0), (-1, 0), (1, 0), (0, -1), (0, 1))
    for enemy in game.visible_enemies:
        if not (0 <= enemy.row < GRID_SIZE and 0 <= enemy.col < GRID_SIZE):
            continue
        for dr, dc in offsets:
            cell = (enemy.row + dr, enemy.col + dc)
            if 0 <= cell[0] < GRID_SIZE and 0 <= cell[1] < GRID_SIZE:
                marked.add(cell)
    marked.difference_update(own)
    limit = max(0, min(MAX_NPCS, game.num_visible_npcs))
    for index in range(limit):
        npc = game.visible_npcs[index]
        if npc.id == 0 or not (
            0 <= npc.pos.row < GRID_SIZE and 0 <= npc.pos.col < GRID_SIZE
        ):
            continue
        for dr, dc in offsets:
            cell = (npc.pos.row + dr, npc.pos.col + dc)
            if 0 <= cell[0] < GRID_SIZE and 0 <= cell[1] < GRID_SIZE:
                marked.add(cell)
    for row, col in marked:
        value = result.grid[row][col]
        if value > 0:
            result.grid[row][col] = value * 7 // 20
    return result


def logged_output(turn: dict[str, Any]) -> GameOutput | None:
    own = player(turn["end"], 1)
    first = own["units"][0].get("actions", [])
    second = own["units"][1].get("actions", [])
    actions = first + second
    if len(actions) != STEPS:
        return None
    result = GameOutput()
    for index, action in enumerate(actions):
        result.actions[index] = action
    result.k = len(first)
    result.order = own.get("order", 0)
    result.vp = 0
    return result


def compare(match: MatchLog, baseline: Strategy, candidate: Strategy) -> None:
    changed = 0
    totals = [[0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0]]
    immediate = [0, 0, 0]  # candidate worse/equal/better
    late_values = [0, 0]
    late_immediate = [0, 0, 0]
    calibration = [0, 0, 0, 0]  # raw, modeled, actual, sample count
    calibration_error = [0, 0]
    vp_changes = 0
    bomb_seen: dict[tuple[int, int], int] = {}
    for turn in match.turns:
        game = build_input(turn)
        for row in range(GRID_SIZE):
            for col in range(GRID_SIZE):
                value = game.grid[row][col]
                cell = (row, col)
                if value == -3:
                    bomb_seen[cell] = game.round
                elif value != -5:
                    bomb_seen.pop(cell, None)
        remembered_bombs = {
            cell
            for cell, seen_round in bomb_seen.items()
            if seen_round // 20 == game.round // 20
        }
        outputs = (
            baseline.decide(ctypes.byref(game)),
            candidate.decide(ctypes.byref(game)),
        )
        if any(
            (list(outputs[0].actions), outputs[0].k, outputs[0].order)[index]
            != (list(outputs[1].actions), outputs[1].k, outputs[1].order)[index]
            for index in range(3)
        ):
            changed += 1
        vp_changes += outputs[0].vp != outputs[1].vp
        evaluations = [
            evaluate(game, output, remembered_bombs) for output in outputs
        ]
        dispatch = turn["end"].get("dispatch_order", [])
        is_late = (
            1 in dispatch and 2 in dispatch and dispatch.index(1) > dispatch.index(2)
        )
        scoring_game = late_hand_input(game) if is_late else game
        late_evaluations = [
            evaluate(scoring_game, output, remembered_bombs) for output in outputs
        ]
        late_round_values = []
        for index, value in enumerate(late_evaluations):
            net = value.pickup - value.known_loss - value.remembered_bomb_loss
            late_values[index] += net
            late_round_values.append(net)
        late_immediate[
            (late_round_values[1] > late_round_values[0])
            - (late_round_values[1] < late_round_values[0])
            + 1
        ] += 1
        if is_late:
            recorded = logged_output(turn)
            if recorded is not None:
                raw = evaluate(game, recorded).pickup
                modeled = evaluate(scoring_game, recorded).pickup
                actual = sum(
                    unit.get("pickup", 0)
                    for unit in player(turn["end"], 1)["units"]
                )
                calibration[0] += raw
                calibration[1] += modeled
                calibration[2] += actual
                calibration[3] += 1
                calibration_error[0] += abs(raw - actual)
                calibration_error[1] += abs(modeled - actual)
        for index, value in enumerate(evaluations):
            totals[index][0] += value.pickup
            totals[index][1] += value.known_loss
            totals[index][2] += value.remembered_bomb_loss
            totals[index][3] += value.remembered_bomb_entries
            totals[index][4] += value.fog_moves
            totals[index][5] += value.blocked
        value0 = (
            evaluations[0].pickup
            - evaluations[0].known_loss
            - evaluations[0].remembered_bomb_loss
        )
        value1 = (
            evaluations[1].pickup
            - evaluations[1].known_loss
            - evaluations[1].remembered_bomb_loss
        )
        immediate[(value1 > value0) - (value1 < value0) + 1] += 1
    print(f"match={match.path} rounds={len(match.turns)} changed={changed} vp_changed={vp_changes}")
    for label, values in zip(("baseline", "candidate"), totals):
        print(
            f"  {label}: visible_pickup={values[0]} known_loss={values[1]} "
            f"remembered_bomb_risk={values[2]} "
            f"remembered_bomb_entries={values[3]} fog_moves={values[4]} "
            f"blocked={values[5]}"
        )
    print(
        f"  immediate_value: worse={immediate[0]} equal={immediate[1]} "
        f"better={immediate[2]}"
    )
    print(
        f"  late_hand_model: baseline={late_values[0]} "
        f"candidate={late_values[1]} delta={late_values[1] - late_values[0]:+d} "
        f"worse={late_immediate[0]} equal={late_immediate[1]} "
        f"better={late_immediate[2]}"
    )
    print(
        f"  model_calibration: late_rounds={calibration[3]} "
        f"actual_pickup={calibration[2]} raw_prediction={calibration[0]} "
        f"modeled_prediction={calibration[1]} "
        f"raw_abs_error={calibration_error[0]} "
        f"modeled_abs_error={calibration_error[1]}"
    )
    return totals, immediate, late_values, late_immediate


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("candidate", type=pathlib.Path)
    parser.add_argument("logs", nargs="+", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        baseline = Strategy(args.baseline)
        candidate = Strategy(args.candidate)
        aggregate = [[0] * 6, [0] * 6]
        immediate = [0, 0, 0]
        late_values = [0, 0]
        late_immediate = [0, 0, 0]
        for path in args.logs:
            totals, match_immediate, match_late, match_late_immediate = compare(
                read_log(path), baseline, candidate
            )
            for strategy in range(2):
                for metric in range(6):
                    aggregate[strategy][metric] += totals[strategy][metric]
            for index in range(3):
                immediate[index] += match_immediate[index]
                late_immediate[index] += match_late_immediate[index]
            for strategy in range(2):
                late_values[strategy] += match_late[strategy]
        if len(args.logs) > 1:
            print("aggregate:")
            for label, values in zip(("baseline", "candidate"), aggregate):
                print(
                    f"  {label}: visible_pickup={values[0]} "
                    f"known_loss={values[1]} "
                    f"remembered_bomb_risk={values[2]} "
                    f"remembered_bomb_entries={values[3]} "
                    f"fog_moves={values[4]} blocked={values[5]}"
                )
            print(
                f"  immediate_value: worse={immediate[0]} "
                f"equal={immediate[1]} better={immediate[2]}"
            )
            print(
                f"  late_hand_model: baseline={late_values[0]} "
                f"candidate={late_values[1]} "
                f"delta={late_values[1] - late_values[0]:+d} "
                f"worse={late_immediate[0]} equal={late_immediate[1]} "
                f"better={late_immediate[2]}"
            )
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
