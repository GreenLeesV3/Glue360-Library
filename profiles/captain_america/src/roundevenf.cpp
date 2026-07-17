// PPC rounding instruction helper for clang-cl on Windows.

#include <cmath>

extern "C" float roundevenf(float x) {
  float result = std::round(x);
  if (std::abs(x - result) == 0.5f) {
    const float lower = std::floor(x);
    result = std::fmod(lower, 2.0f) == 0.0f ? lower : lower + 1.0f;
  }
  return result;
}
