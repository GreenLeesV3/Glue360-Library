// roundevenf - round to nearest, ties to even (C99/IEEE 754)
// MSVC CRT does not provide this; clang-cl emits calls to it for PPC
// rounding instructions (frin/frip/frim/friz) mapped via __builtin_rintf.

#include <cmath>
#include <cfenv>

extern "C" float roundevenf(float x) {
  float result;
  // Save/restore rounding mode in case it was changed by mtfsf
  int save_round = std::fegetround();
  std::fesetround(FE_TONEAREST);
  result = std::nearbyintf(x);
  std::fesetround(save_round);
  return result;
}
