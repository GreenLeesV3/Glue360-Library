// roundevenf.cpp - PPC rounding instruction helper
// Provides roundevenf() for frin/frip/frim/friz PPC instructions
// that clang-cl compiles to a libm call not available on Windows.

#include <cmath>
#include <cstdint>

extern "C" float roundevenf(float x) {
    // Round to nearest, ties to even (banker's rounding)
    // This matches PPC's friN (round to nearest even) instruction
    float r = std::round(x);
    // Check for tie-breaking case (x.5)
    float diff = std::abs(x - r);
    if (diff == 0.5f) {
        // Round to even
        float floor_val = std::floor(x);
        if (std::fmod(floor_val, 2.0f) != 0.0f) {
            r = floor_val + 1.0f;  // Round up to even
        } else {
            r = floor_val;  // Already even
        }
    }
    return r;
}
