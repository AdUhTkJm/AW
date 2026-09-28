#ifndef POD_VECTOR_H
#define POD_VECTOR_H

#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace aw {

// Vector for POD types.
// Cannot hold more than 2^32 elements, but we never need that much.
template<typename T>
class PodVector {
  using uint = uint32_t;

  uint growth(uint target) const noexcept {
    return std::max(target, cap + cap / 2);
  }

  uint cap;
  uint sz;
  // We guarantee that dat will never be null.
  T *dat;
public:
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;
  using size_type = uint;
  using difference_type = std::make_signed_t<uint>;

  PodVector() noexcept: cap(8), sz(0), dat((T*) malloc(cap * sizeof(T))) {}

  explicit PodVector(size_type count) noexcept:
    cap(std::max<uint>(count, 8)), sz(count), dat((T*) (malloc(cap * sizeof(T)))) {}

  static PodVector zeroes(size_type count) noexcept {
    PodVector v(count);
    memset(v.dat, 0, v.sz * sizeof(T));
    return v;
  }

  PodVector(T *begin, T *end): cap(end - begin), sz(cap), dat((T*) malloc(cap * sizeof(T))) {
    memcpy(dat, begin, sz * sizeof(T));
  }

  ~PodVector() noexcept {
    free(dat);
  }

  PodVector(const PodVector &other) noexcept : cap(other.sz), sz(other.sz) {
    // malloc(0) is non-null under C++20 on gcc, clang and msvc.
    dat = (T*) malloc(sz * sizeof(T));
    memcpy(dat, other.dat, sz * sizeof(T));
  }

  PodVector &operator=(const PodVector &other) noexcept {
    [[unlikely]]
    if (this == &other)
      return *this;
    
    if (other.sz > cap) {
      free(dat);
      cap = other.sz;
      dat = (T*) malloc(cap * sizeof(T));
    }
    if ((sz = other.sz) > 0)
      memcpy(dat, other.dat, sz * sizeof(T));
    
    return *this;
  }

  PodVector(PodVector &&other) noexcept:
    cap(other.cap), sz(other.sz), dat(other.dat) {
    other.dat = (T*) malloc(0);
    other.cap = 0;
    other.sz = 0;
  }

  PodVector& operator=(PodVector &&other) noexcept {
    [[unlikely]]
    if (this == &other)
      return *this;

    free(dat);
    dat = other.dat;
    cap = other.cap;
    sz = other.sz;

    other.dat = (T*) malloc(0);
    other.cap = 0;
    other.sz = 0;
    return *this;
  }

  void reserve(size_t newcap) noexcept {
    [[unlikely]]
    if (newcap <= cap)
      return;

    reserve_unchecked(newcap);
  }

  void reserve_unchecked(size_t newcap) noexcept {
    T *newdat = (T*) std::realloc(dat, newcap * sizeof(T));
    dat = newdat;
    cap = newcap;
  }

  void resize(size_t newsz) noexcept {
    if (newsz > cap)
      reserve_unchecked(growth(newsz));
    
    sz = newsz;
  }

  void zero_out(size_t newsz) noexcept {
    if (newsz > cap) {
      dat = (T*) calloc(sz = cap = newsz, sizeof(T));
      return;
    }

    memset(dat, (sz = newsz) * sizeof(T), 0);
  }

  void clear() noexcept {
    sz = 0;
  }

  void push_back(T val) noexcept {
    if (sz >= cap) {
      reserve(growth(sz + 1));
    }
    dat[sz++] = val;
  }

  void push_back_unchecked(T val) noexcept {
    dat[sz++] = val;
  }

  void append_range(const T *src, size_type count) noexcept {
    if (count == 0) return;
    if (sz + count > cap)
      reserve_unchecked(growth(sz + count));
    
    memcpy(dat + sz, src, count * sizeof(T));
    sz += count;
  }

  void erase_unordered(size_type index) noexcept {
    dat[index] = dat[--sz];
  }

  void erase(size_type index) noexcept {
    memmove(dat + index, dat + index + 1, (sz - index - 1) * sizeof(T));
    --sz;
  }

  reference operator[](size_type index) noexcept { return dat[index]; }
  T operator[](size_type index) const noexcept { return dat[index]; }

  reference front() noexcept { return dat[0]; }
  T front() const noexcept { return dat[0]; }
  reference back() noexcept { return dat[sz - 1]; }
  T back() const noexcept { return dat[sz - 1]; }

  pointer data() noexcept { return dat; }
  const_pointer data() const noexcept { return dat; }

  size_type size() const noexcept { return sz; }
  size_type capacity() const noexcept { return cap; }
  bool empty() const noexcept { return sz == 0; }

  pointer begin() noexcept { return dat; }
  pointer end() noexcept { return dat + sz; }
  const_pointer begin() const noexcept { return dat; }
  const_pointer end() const noexcept { return dat + sz; }
};

template<class T>
using vector = std::conditional_t<
  std::is_integral_v<T>,
  PodVector<T>,
  std::vector<T>
>;

}

#endif
