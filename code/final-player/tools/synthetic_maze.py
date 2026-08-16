#!/usr/bin/env python3
"""Deterministic coverage proxy for the logged maze map.

This is deliberately not presented as an official engine.  It preserves the
documented movement/pickup rules, uses the static wall/high-yield cells exposed
by a log header, and generates roughly the observed central/outer gold volume.
It omits opponents, NPC movement, bombs and the unpublished random process.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import pathlib
import random
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOLS = ROOT / "code" / "tools"
sys.path.insert(0, str(TOOLS))
from replay_compare import (  # noqa: E402
    GameInput,
    NpcInfo,
    Position,
    RegionStat,
    Strategy,
)

MOVES = ((-1, 0), (1, 0), (0, -1), (0, 1))


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


def load_board(path: pathlib.Path) -> list[list[str]]:
    with path.open("r", encoding="utf-8") as stream:
        next(stream)
        return json.loads(next(stream))


def simulate(
    strategy_path: pathlib.Path,
    board: list[list[str]],
    seed: int,
) -> tuple[int, tuple[int, int], tuple[int, int]]:
    strategy = Strategy(strategy_path)
    rng = random.Random(seed)
    walls = {
        (row, col)
        for row, values in enumerate(board)
        for col, value in enumerate(values)
        if value == "1"
    }
    hotspots = [
        (row, col)
        for row, values in enumerate(board)
        for col, value in enumerate(values)
        if value == "2"
    ]
    central = [
        (row, col)
        for row in range(4, 13)
        for col in range(4, 13)
        if (row, col) not in walls
    ]
    ground = [[0] * 17 for _ in range(17)]
    positions = [[0, 16], [16, 0]]
    held = [0, 0]
    window_generated = [0] * 5
    window_collected = [0] * 5

    for round_number in range(500):
        for _ in range(3):
            row, col = rng.choice(central)
            amount = rng.randint(1, 4)
            ground[row][col] += amount
            window_generated[0] += amount
        if round_number >= 5 and (round_number - 5) % 10 == 0:
            row, col = rng.choice(hotspots)
            amount = rng.randint(75, 115)
            ground[row][col] += amount
            window_generated[region((row, col)) - 1] += amount
            for _ in range(2):
                row, col = rng.choice(hotspots)
                amount = rng.randint(1, 10)
                ground[row][col] += amount
                window_generated[region((row, col)) - 1] += amount

        game = GameInput()
        game.round = round_number
        for row in range(17):
            for col in range(17):
                game.grid[row][col] = -5
        for unit, position in enumerate(positions):
            game.my_units[unit] = Position(*position)
            game.my_units_gold[unit] = held[unit]
            for row in range(max(0, position[0] - 2), min(17, position[0] + 3)):
                for col in range(
                    max(0, position[1] - 2), min(17, position[1] + 3)
                ):
                    game.grid[row][col] = (
                        -1 if (row, col) in walls else ground[row][col]
                    )
        game.gold_opp = 0
        for index in range(2):
            game.visible_enemies[index] = Position(-1, -1)
        game.num_visible_npcs = 0
        for index in range(7):
            game.visible_npcs[index] = NpcInfo(0, Position(-1, -1))

        snapshot = round_number > 0 and round_number % 5 == 0
        game.snapshot_valid = int(snapshot)
        game.snapshot.window_begin = round_number - 5 if snapshot else -1
        game.snapshot.window_end = round_number - 1 if snapshot else -1
        if snapshot:
            occupants = [0] * 5
            remaining = [0] * 5
            for position in positions:
                occupants[region(tuple(position)) - 1] += 1
            for row in range(17):
                for col in range(17):
                    remaining[region((row, col)) - 1] += ground[row][col]
            for index in range(5):
                game.snapshot.regions[index] = RegionStat(
                    index + 1,
                    0,
                    0,
                    window_generated[index],
                    window_collected[index],
                    remaining[index],
                    occupants[index],
                )
            window_generated = [0] * 5
            window_collected = [0] * 5

        output = strategy.decide(ctypes.byref(game))
        action_lists = (
            list(output.actions[: output.k]),
            list(output.actions[output.k :]),
        )
        for phase in range(2):
            unit = output.order if phase == 0 else output.order ^ 1
            for action in action_lists[unit]:
                if action == 4:
                    continue
                dr, dc = MOVES[action]
                next_position = [
                    positions[unit][0] + dr,
                    positions[unit][1] + dc,
                ]
                if (
                    not (0 <= next_position[0] < 17 and 0 <= next_position[1] < 17)
                    or tuple(next_position) in walls
                    or next_position == positions[unit ^ 1]
                ):
                    continue
                positions[unit] = next_position
                row, col = next_position
                value = ground[row][col]
                if value > 0:
                    pickup = (value * 65 + 99) // 100
                    ground[row][col] -= pickup
                    held[unit] += pickup
                    window_collected[region((row, col)) - 1] += pickup
    return sum(held), (held[0], held[1]), (tuple(positions[0]), tuple(positions[1]))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("strategies", nargs="+", type=pathlib.Path)
    parser.add_argument(
        "--map-log",
        type=pathlib.Path,
        default=ROOT / "new-logs" / "game_372567.log",
    )
    parser.add_argument("--games", type=int, default=20)
    args = parser.parse_args(argv)
    if args.games <= 0:
        parser.error("--games must be positive")
    board = load_board(args.map_log)
    for path in args.strategies:
        results = [simulate(path, board, seed) for seed in range(args.games)]
        totals = [result[0] for result in results]
        print(
            f"strategy={path} games={args.games} "
            f"mean={statistics.mean(totals):.1f} "
            f"median={statistics.median(totals):.1f} "
            f"min={min(totals)} max={max(totals)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
