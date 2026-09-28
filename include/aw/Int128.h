#ifndef AW_INT128_H
#define AW_INT128_H

#include <cstdint>

// A signed 128-bit integer.
//
// The planner multiplies two int64_t balances and adds the products up. Those
// products overflow 64 bits but fit 128 comfortably (|a * b| <= 2^126), so the
// overflow checks and the exact balance comparison need a wider type than any
// built-in one. That work used to fall back to `long double`, which is not a
// portable choice: x86-64 Linux gives it a 64-bit mantissa, MSVC gives it the
// same 64-bit mantissa as `double`, and 64 bits cannot hold an int64_t product
// exactly. Keeping the arithmetic integral makes it exact on every platform.
//
// GCC and Clang have a native type, so aw::int128 is an alias there and the
// Linux codegen is unchanged. MSVC has none, so it gets a small two's
// complement wrapper over the _umul128 intrinsic.
//
// Only what the callers use is provided: 64x64 -> 128 multiplication, additions
// of such products, negation, comparison and a truncating conversion back to
// int64_t. There is deliberately no division, shifting or increment.

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
  // Implicit on purpose: `(aw::int128) amount * times` and comparisons against
  // plain int64_t values have to keep the shape they have with the native type.
  constexpr int128(int64_t value = 0) noexcept
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
