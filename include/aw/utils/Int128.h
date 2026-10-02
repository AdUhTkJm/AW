#ifndef AW_INT128_H
#define AW_INT128_H

#include <cstdint>

// A signed 128-bit integer.
//
// GCC and Clang have a native type, so aw::int128 is an alias there.
// MSVC has none, so we must write one.

#if !defined(_MSC_VER) || defined(__clang__)

namespace aw {
using int128 = __int128;
}

#else

#include <intrin.h>

namespace aw {

class int128 {
  // Two's complement, little endian limb order: value = hi_ * 2^64 + lo_.
  //
  // hi_ is kept unsigned so that addition and subtraction wrap around instead
  // of overflowing a signed type, which is undefined behavior; comparisons
  // reinterpret it as int64_t.
  uint64_t lo_;
  uint64_t hi_;

  static constexpr int128 fromLimbs(uint64_t hi, uint64_t lo) noexcept {
    int128 value;
    value.lo_ = lo;
    value.hi_ = hi;
    return value;
  }

public:
  int128() = default;

  // Implicit on purpose: `(aw::int128) amount * times` and comparisons against
  // plain int64_t values have to keep the shape they have with the native type.
  constexpr int128(int64_t value) noexcept
      : lo_((uint64_t) value), hi_(value < 0 ? UINT64_MAX : 0) {}

  friend int128 operator*(int128 a, int128 b) noexcept {
    // Low 128 bits of a * b. Writing the product out as
    //   hi_a*hi_b*2^128 + (hi_a*lo_b + lo_a*hi_b)*2^64 + lo_a*lo_b
    // shows that the first term vanishes modulo 2^128 and that only the low
    // limb of the cross terms survives, so plain wrapping 64-bit arithmetic is
    // enough. Callers only multiply values that came from int64_t, so the true
    // product never exceeds 128 bits and nothing is lost.
    uint64_t hi;
    const uint64_t lo = _umul128(a.lo_, b.lo_, &hi);
    hi += a.hi_ * b.lo_ + a.lo_ * b.hi_;
    return fromLimbs(hi, lo);
  }

  friend int128 operator+(int128 a, int128 b) noexcept {
    const uint64_t lo = a.lo_ + b.lo_;
    return fromLimbs(a.hi_ + b.hi_ + (lo < a.lo_ ? 1 : 0), lo);
  }

  friend int128 operator-(int128 value) noexcept {
    const uint64_t lo = ~value.lo_ + 1;
    return fromLimbs(~value.hi_ + (lo == 0 ? 1 : 0), lo);
  }

  friend int128 operator-(int128 a, int128 b) noexcept { return a + (-b); }

  int128 &operator+=(int128 other) noexcept {
    *this = *this + other;
    return *this;
  }

  int128 &operator-=(int128 other) noexcept {
    *this = *this - other;
    return *this;
  }

  friend bool operator==(int128 a, int128 b) noexcept {
    return a.lo_ == b.lo_ && a.hi_ == b.hi_;
  }

  friend bool operator!=(int128 a, int128 b) noexcept { return !(a == b); }

  friend bool operator<(int128 a, int128 b) noexcept {
    const int64_t ahi = (int64_t) a.hi_;
    const int64_t bhi = (int64_t) b.hi_;
    return ahi != bhi ? ahi < bhi : a.lo_ < b.lo_;
  }

  friend bool operator>(int128 a, int128 b) noexcept { return b < a; }
  friend bool operator<=(int128 a, int128 b) noexcept { return !(b < a); }
  friend bool operator>=(int128 a, int128 b) noexcept { return !(a < b); }

  // Truncating, like the implicit conversion of the native type.
  explicit constexpr operator int64_t() const noexcept { return (int64_t) lo_; }
};

}  // namespace aw

#endif

#endif
