#include "../strategy_core.h"

extern "C" __attribute__((visibility("default")))
GameOutput moveDecision(const GameInput* input) {
    return final_player::decide(
        input, final_player::Profile::kAdaptiveNoBlockInference);
}
