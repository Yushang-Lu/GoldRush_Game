#!/usr/bin/env python3
"""Read GoldRush JSON-lines match logs and print reproducible summaries.

The first line contains player names, the second line the underlying map, and
the following lines contain one round each.  This tool is offline-only: the
submitted strategy never reads files or logs.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import sys
from collections import Counter
from dataclasses import dataclass
from typing import Any, Iterable


@dataclass(frozen=True)
class MatchLog:
    path: pathlib.Path
    players: dict[str, str]
    board: list[list[str]]
    turns: list[dict[str, Any]]


def read_log(path: pathlib.Path) -> MatchLog:
    with path.open("r", encoding="utf-8") as stream:
        try:
            players = json.loads(next(stream))
            board = json.loads(next(stream))
            turns = [json.loads(line) for line in stream if line.strip()]
        except (StopIteration, json.JSONDecodeError) as error:
            raise ValueError(f"{path}: invalid GoldRush log: {error}") from error
    if not isinstance(players, dict) or not isinstance(board, list):
        raise ValueError(f"{path}: invalid header")
    rounds = [turn.get("round") for turn in turns]
    if rounds != list(range(len(turns))):
        raise ValueError(f"{path}: rounds are not contiguous from zero")
    return MatchLog(path, players, board, turns)


def player(state: dict[str, Any], player_id: int) -> dict[str, Any]:
    for value in state["players"]:
        if value["id"] == player_id:
            return value
    raise ValueError(f"player {player_id} missing from state")


def percentile(values: Iterable[int], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    return ordered[int(fraction * (len(ordered) - 1))] / 1000.0


def vision_choice(start: dict[str, Any], end: dict[str, Any]) -> int:
    delta = end.get("vision_spent", 0) - start.get("vision_spent", 0)
    return 1 if delta == 2 else 2 if delta == 3 else 0


def summarize(match: MatchLog) -> None:
    turns = match.turns
    print(f"match={match.path} players={match.players} rounds={len(turns)}")
    match_burned = sum(max(0, int(turn["end"].get("burned", 0))) for turn in turns)
    tramples = sum(len(turn["end"].get("trample_events", [])) for turn in turns)
    overlaps = sum(len(turn["end"].get("overlap_events", [])) for turn in turns)
    print(
        f"  match_events: burned={match_burned} tramples={tramples} "
        f"overlaps={overlaps}"
    )
    final_state = turns[-1]["end"] if turns else {"players": []}
    net_scores: dict[int, int] = {}
    for player_id in (1, 2):
        costs: list[int] = []
        pickup = 0
        inferred_loss = 0
        first = 0
        actions = 0
        stays = 0
        purchases: Counter[int] = Counter()
        for turn in turns:
            start = player(turn["start"], player_id)
            end = player(turn["end"], player_id)
            costs.append(end.get("cost", 0))
            pickup += sum(unit.get("pickup", 0) for unit in end["units"])
            if player_id == 1:
                inferred_loss += max(0, start.get("gold", 0) + sum(
                    unit.get("pickup", 0) for unit in end["units"]
                ) - end.get("gold", 0))
            flat_actions = [
                action
                for unit in end["units"]
                for action in unit.get("actions", [])
            ]
            actions += len(flat_actions)
            stays += flat_actions.count(4)
            purchases[vision_choice(start, end)] += 1
            dispatch = turn["end"].get("dispatch_order", [])
            if player_id in dispatch:
                other = 2 if player_id == 1 else 1
                if other in dispatch and dispatch.index(player_id) < dispatch.index(other):
                    first += 1
        final = player(final_state, player_id)
        gold = final.get("gold", 0)
        spent = final.get("vision_spent", 0)
        net_scores[player_id] = gold - spent
        print(
            f"  p{player_id}: gross={gold} vision={spent} net={gold - spent} "
            f"recorded_pickup={pickup} first={first}/{len(turns)} "
            f"recorded_actions={actions} stays={stays} "
            f"vp={dict(sorted(purchases.items()))}"
        )
        if player_id == 1:
            print(f"    p1_inferred_loss={inferred_loss}")
        print(
            "    latency_us: "
            f"p50={percentile(costs, 0.50):.3f} "
            f"p90={percentile(costs, 0.90):.3f} "
            f"p99={percentile(costs, 0.99):.3f} "
            f"max={(max(costs) / 1000.0 if costs else 0.0):.3f}"
        )
    if len(net_scores) == 2:
        winner = "draw" if net_scores[1] == net_scores[2] else (
            "p1" if net_scores[1] > net_scores[2] else "p2"
        )
        margin = net_scores[1] - net_scores[2]
        print(f"  result: winner={winner} p1_net_margin={margin:+d}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="summarize GoldRush 2.0 JSON-lines match logs"
    )
    parser.add_argument("logs", nargs="+", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        matches = [read_log(path) for path in args.logs]
    except (OSError, ValueError) as error:
        parser.error(str(error))
    for match in matches:
        summarize(match)
    if len(matches) > 1:
        maps_equal = all(match.board == matches[0].board for match in matches[1:])
        opponents_equal = all(match.players == matches[0].players for match in matches[1:])
        print(f"cross_match: same_map={maps_equal} same_players={opponents_equal}")
        p1_wins = p2_wins = draws = 0
        for match in matches:
            final = match.turns[-1]["end"]
            scores = []
            for player_id in (1, 2):
                value = player(final, player_id)
                scores.append(value.get("gold", 0) - value.get("vision_spent", 0))
            if scores[0] > scores[1]:
                p1_wins += 1
            elif scores[0] < scores[1]:
                p2_wins += 1
            else:
                draws += 1
        print(
            f"cross_match_result: p1_wins={p1_wins} p2_wins={p2_wins} "
            f"draws={draws}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
