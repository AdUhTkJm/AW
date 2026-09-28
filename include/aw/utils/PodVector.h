#ifndef POD_VECTOR_H
#define POD_VECTOR_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

namespace aw {

// Vector for integers.
// Cannot hold more than 2^32 elements, but we never need that much.
// Works for every POD theoretically, but elements are passed in value, since
// integers are small enough to pass in a single register.
template<typename T>
class PodVector {
  static_assert(std::is_integral_v<T>,
                "PodVector requires a trivially copyable element type");

  using uint = uint32_t;

  uint growth(uint target) const noexcept {
    return std::max(target, cap + cap / 2);
  }

  // malloc(0) might return nullptr.
  static T *allocate(uint count) noexcept {
    [[unlikely]]
    if (count == 0)
      return (T*) malloc(sizeof(T));

    return (T*) malloc(count * sizeof(T));
  }

  static T *reallocate(T *ptr, uint count) noexcept {
    [[unlikely]]
    if (count == 0)
      return (T*) realloc(ptr, sizeof(T));

    return (T*) realloc(ptr, count * sizeof(T));
  }

  // The range overloads take forward iterators,
  // so PodVector(n, value) can never be ambiguous with PodVector(first, last).
  template<typename It>
  static constexpr bool isRange = std::forward_iterator<It>;

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
  using iterator = pointer;
  using const_iterator = const_pointer;

  PodVector() noexcept: cap(8), sz(0), dat(allocate(cap)) {}

  explicit PodVector(size_type count) noexcept:
    cap(std::max<uint>(count, 8)), sz(count), dat(allocate(cap)) {}

  PodVector(size_type count, T value) noexcept:
    cap(std::max<uint>(count, 8)), sz(count), dat(allocate(cap)) {
    for (size_type i = 0; i < sz; i++)
      dat[i] = value;
  }

  static PodVector zeroes(size_type count) noexcept {
    PodVector v(count);
    memset(v.dat, 0, v.sz * sizeof(T));
    return v;
  }

  PodVector(std::initializer_list<T> init) noexcept:
    cap(std::max<uint>(init.size(), 8)), sz(init.size()), dat(allocate(cap)) {
    size_type i = 0;
    for (T value : init)
      dat[i++] = value;
  }

  template<typename It>
    requires (isRange<It>)
  PodVector(It first, It last) noexcept:
    cap((uint) std::distance(first, last)), sz(cap), dat(allocate(cap)) {
    size_type i = 0;
    for (It it = first; it != last; it++)
      dat[i++] = *it;
  }

  ~PodVector() noexcept {
    free(dat);
  }

  PodVector(const PodVector &other) noexcept : cap(other.sz), sz(other.sz), dat(allocate(cap)) {
    memcpy(dat, other.dat, sz * sizeof(T));
  }

  PodVector &operator=(const PodVector &other) noexcept {
    [[unlikely]]
    if (this == &other)
      return *this;

    if (other.sz > cap)
      reserve_unchecked(other.sz);
    if ((sz = other.sz) > 0)
      memcpy(dat, other.dat, sz * sizeof(T));

    return *this;
  }

  PodVector(PodVector &&other) noexcept:
    cap(other.cap), sz(other.sz), dat(other.dat) {
    other.dat = allocate(1);
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

    other.dat = allocate(1);
    other.cap = 0;
    other.sz = 0;
    return *this;
  }

  void reserve(size_type newcap) noexcept {
    [[unlikely]]
    if (newcap <= cap)
      return;

    reserve_unchecked(newcap);
  }

  void reserve_unchecked(size_type newcap) noexcept {
    dat = reallocate(dat, newcap);
    cap = newcap;
  }

  void resize(size_type newsz) noexcept {
    if (newsz > cap)
      reserve_unchecked(growth(newsz));

    sz = newsz;
  }

  // Unlike std::vector::resize, this value-initializes only the new elements
  // when the vector grows; shrinking leaves the prefix untouched.
  void resize(size_type newsz, T value) noexcept {
    const size_type old = sz;
    if (newsz > cap)
      reserve_unchecked(growth(newsz));

    sz = newsz;
    for (size_type i = old; i < sz; i++)
      dat[i] = value;
  }

  void zero_out(size_type newsz) noexcept {
    if (newsz > cap)
      reserve_unchecked(growth(newsz));

    sz = newsz;
    memset(dat, 0, sz * sizeof(T));
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

  void pop_back() noexcept {
    --sz;
  }

  void append_range(const T *src, size_type count) noexcept {
    if (count == 0) return;
    if (sz + count > cap)
      reserve_unchecked(growth(sz + count));

    memcpy(dat + sz, src, count * sizeof(T));
    sz += count;
  }

  void assign(size_type count, T value) noexcept {
    if (count > cap)
      reserve_unchecked(growth(count));

    for (size_type i = 0; i < count; i++)
      dat[i] = value;
    sz = count;
  }

  template<typename It> requires (isRange<It>)
  void assign(It first, It last) noexcept {
    const size_type count = (size_type) std::distance(first, last);
    if (count > cap)
      reserve_unchecked(growth(count));

    size_type i = 0;
    for (It it = first; it != last; it++)
      dat[i++] = *it;
    sz = count;
  }

  void assign(std::initializer_list<T> init) noexcept {
    assign(init.begin(), init.end());
  }

  pointer insert(pointer pos, T value) noexcept {
    const size_type index = (size_type) (pos - dat);
    if (sz >= cap)
      reserve_unchecked(growth(sz + 1));

    pos = dat + index;
    memmove(pos + 1, pos, (sz - index) * sizeof(T));
    *pos = value;
    sz++;
    return pos;
  }

  pointer insert(pointer pos, size_type count, T value) noexcept {
    if (count == 0)
      return pos;

    const size_type index = (size_type) (pos - dat);
    if (sz + count > cap)
      reserve_unchecked(growth(sz + count));

    pos = dat + index;
    memmove(pos + count, pos, (sz - index) * sizeof(T));
    for (size_type i = 0; i < count; i++)
      pos[i] = value;
    sz += count;
    return pos;
  }

  template<typename It>
    requires (isRange<It>)
  pointer insert(pointer pos, It first, It last) noexcept {
    const size_type index = (size_type) (pos - dat);
    const size_type count = (size_type) std::distance(first, last);
    if (count == 0)
      return dat + index;

    if (sz + count > cap)
      reserve_unchecked(growth(sz + count));

    pos = dat + index;
    memmove(pos + count, pos, (sz - index) * sizeof(T));
    size_type i = 0;
    for (It it = first; it != last; it++)
      pos[i++] = *it;
    sz += count;
    return pos;
  }

  void erase_unordered(size_type index) noexcept {
    dat[index] = dat[--sz];
  }

  void erase(size_type index) noexcept {
    memmove(dat + index, dat + index + 1, (sz - index - 1) * sizeof(T));
    --sz;
  }

  pointer erase(pointer pos) noexcept {
    const size_type index = (size_type) (pos - dat);
    memmove(dat + index, dat + index + 1, (sz - index - 1) * sizeof(T));
    --sz;
    return dat + index;
  }

  pointer erase(pointer first, pointer last) noexcept {
    const size_type index = (size_type) (first - dat);
    const size_type count = (size_type) (last - first);
    if (count == 0)
      return dat + index;

    memmove(dat + index, dat + index + count, (sz - index - count) * sizeof(T));
    sz -= count;
    return dat + index;
  }

  void swap(PodVector &other) noexcept {
    std::swap(cap, other.cap);
    std::swap(sz, other.sz);
    std::swap(dat, other.dat);
  }

  void shrink_to_fit() noexcept {
    if (sz == cap)
      return;

    dat = reallocate(dat, sz);
    cap = sz;
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

  friend bool operator==(const PodVector &a, const PodVector &b) noexcept {
    if (a.sz != b.sz)
      return false;
    for (size_type i = 0; i < a.sz; i++)
      if (a.dat[i] != b.dat[i])
        return false;
    return true;
  }
};

template<class T>
using vector = std::conditional_t<
  std::is_integral_v<T>,
  PodVector<T>,
  std::vector<T>
>;

}

#endif
