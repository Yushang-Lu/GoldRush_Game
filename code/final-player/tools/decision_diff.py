#!/usr/bin/env python3
"""Count exact output differences between two libraries on logged inputs."""

from __future__ import annotations

import argparse
import ctypes
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from log_report import build_input  # noqa: E402

ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "code" / "tools"))
from log_analyzer import read_log  # noqa: E402
from replay_compare import Strategy  # noqa: E402


def encoded(output: object) -> tuple:
    return (
        tuple(output.actions),
        output.k,
        output.order,
        output.vp,
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("candidate", type=pathlib.Path)
    parser.add_argument("logs", nargs="+", type=pathlib.Path)
    parser.add_argument("--own-id", type=int, choices=(1, 2), default=1)
    args = parser.parse_args(argv)
    baseline = Strategy(args.baseline)
    candidate = Strategy(args.candidate)
    total = changed = 0
    for path in args.logs:
        match = read_log(path)
        match_changed = 0
        for turn in match.turns:
            game = build_input(turn, args.own_id)
            first = baseline.decide(ctypes.byref(game))
            second = candidate.decide(ctypes.byref(game))
            difference = encoded(first) != encoded(second)
            changed += difference
            match_changed += difference
            total += 1
        print(f"{path}: rounds={len(match.turns)} changed={match_changed}")
    print(f"aggregate: rounds={total} changed={changed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
