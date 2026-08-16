#include "strategy.h"

extern "C" __attribute__((visibility("default"))) GameOutput moveDecision(
    const GameInput* input) {
    return slow_player::decide(input);
}
