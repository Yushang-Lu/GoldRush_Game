#pragma once

#include "game_api.h"

namespace final_player {

enum class Profile {
    kFastOnly,
    kAlwaysDeep,
    kAdaptive,
    kAdaptiveNoHotspots,
    kAdaptiveNoBlockInference,
};

GameOutput decide(const GameInput* input, Profile profile) noexcept;

}  // namespace final_player
