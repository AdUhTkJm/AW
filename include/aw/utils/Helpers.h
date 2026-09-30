#ifndef HELPERS_H
#define HELPERS_H

#include <span>
#include <cstdint>

namespace aw {


// GCC and Clang have the overflow builtins and define __has_builtin; MSVC has
// neither.
#if defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow) && __has_builtin(__builtin_mul_overflow)
#    define AW_HAVE_OVERFLOW_BUILTINS 1
#  endif
#endif

inline bool addOverflow(int64_t a, int64_t b, int64_t &out) noexcept {
#ifdef AW_HAVE_OVERFLOW_BUILTINS
  return __builtin_add_overflow(a, b, &out);
#else
  if (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)
    return true;
  out = a + b;
  return false;
#endif
}

inline bool mulOverflow(int64_t a, int64_t b, int64_t &out) noexcept {
#ifdef AW_HAVE_OVERFLOW_BUILTINS
  return __builtin_mul_overflow(a, b, &out);
#else
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  const bool over = a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
                          : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b);
  if (over)
    return true;
  out = a * b;
  return false;
#endif

}

}

#endif
