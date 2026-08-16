# Candidate strategies

These wrappers select reproducible profiles from `../strategy_core.cpp`:

- `fast_only`: original `fast-player2` on every map.
- `always_deep`: 28-route wide search on every map; this preserves the user's
  “deep search all the way” alternative without artificial sleeping.
- `adaptive_no_hotspots`: adaptive deep search without the six logged maze
  hotspots, isolating the value of map-specific resource routing.
- `adaptive_no_block_inference`: final adaptive planner without cross-round
  endpoint mismatch handling, useful for counterfactual replay where logged
  positions necessarily came from a different strategy.

Build all profiles with `make -C code/final-player candidates`. The generated
libraries remain separate from the submission artifact `../player.so`.
