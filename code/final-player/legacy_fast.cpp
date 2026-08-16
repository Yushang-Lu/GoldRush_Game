// Build the already server-tested fast-player2 as an internal fallback.
// The linker version script keeps this renamed entry point private.
#define moveDecision legacyMoveDecision
#include "../fast-player2/fast-player2.cpp"
#undef moveDecision
