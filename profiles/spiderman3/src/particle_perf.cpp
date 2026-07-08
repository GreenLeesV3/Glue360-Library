// spiderman3 - Particle performance hooks
// Currently empty — the particle_generator_vfunc8 hook was found to NOT be
// a particle count getter (returning 150 instead of 300 crashed the game).
// The hook mechanism itself works (returning 300 = no crash), but we need
// to find the actual particle count/spawn function via runtime tracing.
// This file is kept in CMakeLists.txt for future hook additions.

#include "generated/default/spiderman3_init.h"
#include <rex/hook.h>

// Placeholder — no hooks active
