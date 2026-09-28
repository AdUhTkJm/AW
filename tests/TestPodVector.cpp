// Unit tests for aw::PodVector. Kept separate from Test.cpp: that file covers
// the planner's behaviour, this one is a pure container conformance suite.
//
// Where semantics overlap with std::vector the test mirrors every operation
// onto a std::vector and compares, so the two implementations must agree.
// The deliberate deviations (uninitialized growth, zero_out, zeroes,
// erase_unordered) are asserted explicitly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include "aw/utils/PodVector.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::cout << "  [FAIL] " << what << '\n';
    failures++;
  }
}

template<typename A, typename B>
bool same(const A &a, const B &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i] != b[i])
      return false;
  return true;
}

// Both vectors must hold identical contents and agree on the boundary
// accessors. `label` is only used for failure reporting.
template<typename A, typename B>
void expectSame(const A &a, const B &b, const char* label) {
  expect(a.size() == b.size(), label);
  expect(same(a, b), label);
  if (a.empty() || b.empty())
    return;
  expect(a.front() == b.front(), label);
  expect(a.back() == b.back(), label);
}

// Deterministic LCG so a failure is reproducible.
struct Rng {
  uint32_t state = 0x12345678u;

  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }

  uint32_t below(uint32_t bound) { return bound == 0 ? 0 : next() % bound; }

  int value() { return (int) below(2001) - 1000; }
};

void testConstruction() {
  std::cout << "[PodVector] construction\n";

  {
    aw::PodVector<int> v;
    expect(v.empty() && v.size() == 0, "default constructed is empty");
    expect(v.data() != nullptr, "default constructed data is non-null");
    expect(v.capacity() >= 8, "default constructed has room to grow");
    expect(v.begin() == v.end(), "empty begin equals end");
  }

  {
    // Count constructor: sized but uninitialized, so only size/capacity are
    // observable without writing first.
    aw::PodVector<int> v(20);
    expect(v.size() == 20, "count constructor sets the size");
    expect(v.capacity() >= 20, "count constructor allocates at least the size");
    for (int i = 0; i < 20; i++)
      v[i] = i * 3;
    for (int i = 0; i < 20; i++)
      expect(v[i] == i * 3, "count constructor elements are writable");

    aw::PodVector<int> zero(0);
    expect(zero.empty() && zero.data() != nullptr, "zero-count construction is empty");
  }

  {
    aw::PodVector<int> v(5, 7);
    std::vector<int> ref(5, 7);
    expectSame(v, ref, "fill constructor matches std::vector");
  }

  {
    aw::PodVector<int> v = {4, 8, 15, 16, 23, 42};
    std::vector<int> ref = {4, 8, 15, 16, 23, 42};
    expectSame(v, ref, "initializer-list construction matches std::vector");
  }

  {
    const int src[] = {1, 2, 3, 4};
    aw::PodVector<int> v(src, src + 4);
    std::vector<int> ref(src, src + 4);
    expectSame(v, ref, "iterator construction matches std::vector");
  }

  {
    std::vector<int> src = {9, 8, 7};
    aw::PodVector<int> v(src.begin(), src.end());
    expectSame(v, src, "construction from std::vector iterators");
  }

  {
    aw::PodVector<int> a;
    aw::PodVector<int> empty(a.begin(), a.end());
    expect(empty.empty() && empty.data() != nullptr, "empty range construction is safe");
  }

  {
    aw::PodVector<uint8_t> v = aw::PodVector<uint8_t>::zeroes(64);
    expect(v.size() == 64, "zeroes sets the size");
    bool allZero = true;
    for (uint8_t x : v)
      allZero = allZero && x == 0;
    expect(allZero, "zeroes zeroes every element");

    aw::PodVector<uint64_t> z = aw::PodVector<uint64_t>::zeroes(0);
    expect(z.empty() && z.data() != nullptr, "zeroes(0) is an empty, non-null vector");
  }
}

void testAlias() {
  std::cout << "[PodVector] alias selection\n";

  static_assert(std::is_same_v<aw::vector<int>, aw::PodVector<int>>);
  static_assert(std::is_same_v<aw::vector<uint8_t>, aw::PodVector<uint8_t>>);
  static_assert(std::is_same_v<aw::vector<int64_t>, aw::PodVector<int64_t>>);
  static_assert(std::is_same_v<aw::vector<uint64_t>, aw::PodVector<uint64_t>>);
  static_assert(std::is_same_v<aw::vector<bool>, aw::PodVector<bool>>);

  // Non-integral elements fall back to std::vector.
  static_assert(std::is_same_v<aw::vector<double>, std::vector<double>>);
  static_assert(std::is_same_v<aw::vector<std::byte>, std::vector<std::byte>>);
  static_assert(std::is_same_v<aw::vector<float>, std::vector<float>>);
  static_assert(std::is_same_v<aw::vector<aw::vector<int>>, std::vector<aw::PodVector<int>>>);

  expect(true, "alias selection");
}

void testPushPop() {
  std::cout << "[PodVector] push_back / pop_back\n";

  aw::PodVector<int> v;
  std::vector<int> ref;
  for (int i = 0; i < 1000; i++) {
    v.push_back(i * i - i);
    ref.push_back(i * i - i);
  }
  expectSame(v, ref, "push_back matches std::vector across growths");
  expect(v.capacity() >= v.size(), "capacity never drops below size");

  while (!ref.empty()) {
    v.pop_back();
    ref.pop_back();
    expect(v.size() == ref.size(), "pop_back shrinks the size");
  }
  expect(v.empty() && v.data() != nullptr, "pop_back down to empty keeps data non-null");

  // push_back_unchecked only writes; it must not grow.
  aw::PodVector<int> u(4, 0);
  const uint32_t cap = u.capacity();
  u.push_back_unchecked(1);
  expect(u.size() == 5 && u.back() == 1, "push_back_unchecked writes the element");
  expect(u.capacity() == cap, "push_back_unchecked does not touch capacity");
}

void testReserveResize() {
  std::cout << "[PodVector] reserve / resize\n";

  {
    aw::PodVector<int> v;
    v.reserve(200);
    expect(v.capacity() >= 200 && v.empty(), "reserve grows capacity only");
    const uint32_t cap = v.capacity();
    v.reserve(10);
    expect(v.capacity() == cap, "reserve never shrinks");
  }

  {
    aw::PodVector<int> v(4, 5);
    v.resize(10, 9);
    std::vector<int> ref(4, 5);
    ref.resize(10, 9);
    expectSame(v, ref, "resize upwards value-initializes only new elements");

    v.resize(2, 9);
    ref.resize(2, 9);
    expectSame(v, ref, "resize downwards truncates");

    v.resize(6, -1);
    ref.resize(6, -1);
    expectSame(v, ref, "resize grows again after shrinking");
  }

  {
    aw::PodVector<int> v(3, 1);
    v.resize(100);
    expect(v.size() == 100 && v.capacity() >= 100, "resize allocates as needed");
    for (int i = 0; i < 3; i++)
      expect(v[i] == 1, "resize preserves the existing prefix");
    for (size_t i = 3; i < v.size(); i++)
      v[i] = (int) i;
    v.resize(3);
    expect(v.size() == 3, "resize can shrink back");
  }

  {
    aw::PodVector<int> v;
    v.zero_out(5);
    expect(v.size() == 5, "zero_out sets the size");
    bool allZero = true;
    for (int x : v)
      allZero = allZero && x == 0;
    expect(allZero, "zero_out zeroes a fresh vector");

    for (int i = 0; i < 5; i++)
      v[i] = i + 1;
    v.zero_out(5);
    allZero = true;
    for (int x : v)
      allZero = allZero && x == 0;
    expect(allZero, "zero_out re-zeroes an existing vector in place");

    // Growing past capacity takes the calloc-like path.
    v.zero_out(500);
    expect(v.size() == 500 && v.capacity() >= 500, "zero_out grows the vector");
    allZero = true;
    for (int x : v)
      allZero = allZero && x == 0;
    expect(allZero, "zero_out zeroes after growing");

    v.zero_out(0);
    expect(v.empty(), "zero_out(0) clears");
  }
}

void testAssign() {
  std::cout << "[PodVector] assign\n";

  {
    aw::PodVector<int> v = {1, 2, 3};
    v.assign(5, 7);
    std::vector<int> ref = {1, 2, 3};
    ref.assign(5, 7);
    expectSame(v, ref, "fill assign matches std::vector");

    v.assign(0, 7);
    ref.assign(0, 7);
    expectSame(v, ref, "assign(0, value) clears");

    v.assign(1000, -3);
    ref.assign(1000, -3);
    expectSame(v, ref, "assign grows past capacity");
  }

  {
    const int src[] = {5, 4, 3, 2, 1};
    aw::PodVector<int> v = {9, 9, 9, 9, 9, 9, 9};
    v.assign(src, src + 5);
    std::vector<int> ref = {9, 9, 9, 9, 9, 9, 9};
    ref.assign(src, src + 5);
    expectSame(v, ref, "range assign matches std::vector");

    std::vector<int> wide(300, 42);
    v.assign(wide.begin(), wide.end());
    ref.assign(wide.begin(), wide.end());
    expectSame(v, ref, "range assign from a wide std::vector");

    v.assign(src, src);
    ref.assign(src, src);
    expectSame(v, ref, "empty range assign clears");
  }

  {
    aw::PodVector<int> v;
    v.assign({2, 4, 6});
    std::vector<int> ref;
    ref.assign({2, 4, 6});
    expectSame(v, ref, "initializer-list assign matches std::vector");
  }

  {
    // assign from a range inside the vector itself must not corrupt.
    aw::PodVector<int> v = {1, 2, 3, 4};
    v.assign(v.begin() + 1, v.end());
    std::vector<int> ref = {2, 3, 4};
    expectSame(v, ref, "self-range assign keeps the source intact");
  }
}

void testInsert() {
  std::cout << "[PodVector] insert\n";

  {
    aw::PodVector<int> v;
    std::vector<int> ref;
    const int probes[] = {0, 1, 2, 3, 10, 11};
    for (size_t k = 0; k < std::size(probes); k++) {
      const size_t pos = (size_t) probes[k] % (ref.size() + 1);
      v.insert(v.begin() + pos, (int) (k * 100));
      ref.insert(ref.begin() + pos, (int) (k * 100));
      expectSame(v, ref, "single insert matches std::vector");
    }
  }

  {
    // Insert once when full, forcing a reallocation.
    aw::PodVector<int> v(8, 1);
    v.insert(v.begin(), 99);
    std::vector<int> ref(8, 1);
    ref.insert(ref.begin(), 99);
    expectSame(v, ref, "insert at begin with reallocation");

    v.insert(v.end(), 77);
    ref.insert(ref.end(), 77);
    expectSame(v, ref, "insert at end with reallocation");

    v.insert(v.begin() + 3, -5);
    ref.insert(ref.begin() + 3, -5);
    expectSame(v, ref, "insert in the middle with reallocation");
  }

  {
    aw::PodVector<int> v = {1, 2, 3};
    v.insert(v.begin() + 1, 3, 9);
    std::vector<int> ref = {1, 2, 3};
    ref.insert(ref.begin() + 1, 3, 9);
    expectSame(v, ref, "fill insert matches std::vector");

    v.insert(v.end(), 0, 5);
    ref.insert(ref.end(), 0, 5);
    expectSame(v, ref, "zero-count fill insert is a no-op");
  }

  {
    const int src[] = {7, 8, 9};
    aw::PodVector<int> v = {1, 2};
    v.insert(v.begin() + 1, src, src + 3);
    std::vector<int> ref = {1, 2};
    ref.insert(ref.begin() + 1, src, src + 3);
    expectSame(v, ref, "range insert matches std::vector");

    std::vector<int> wide(300, 4);
    v.insert(v.end(), wide.begin(), wide.end());
    ref.insert(ref.end(), wide.begin(), wide.end());
    expectSame(v, ref, "range insert across a reallocation");

    v.insert(v.end(), src, src);
    ref.insert(ref.end(), src, src);
    expectSame(v, ref, "empty range insert is a no-op");
  }

  {
    // Return value points at the first inserted element.
    aw::PodVector<int> v = {1, 2, 3};
    int *at = v.insert(v.begin() + 1, 42);
    expect(at == v.begin() + 1 && *at == 42, "single insert returns its position");

    int *first = v.insert(v.begin() + 2, 2, 5);
    expect(first == v.begin() + 2 && v[2] == 5 && v[3] == 5,
           "fill insert returns its position");
  }
}

void testErase() {
  std::cout << "[PodVector] erase\n";

  {
    aw::PodVector<int> v = {0, 1, 2, 3, 4, 5};
    int *next = v.erase(v.begin() + 2);
    std::vector<int> ref = {0, 1, 2, 3, 4, 5};
    ref.erase(ref.begin() + 2);
    expectSame(v, ref, "iterator erase matches std::vector");
    expect(next == v.begin() + 2 && *next == 3, "iterator erase returns the next element");
  }

  {
    aw::PodVector<int> v = {0, 1, 2, 3, 4, 5};
    uint32_t index = 0;
    v.erase(index);
    std::vector<int> ref = {1, 2, 3, 4, 5};
    expectSame(v, ref, "index erase removes the front");
  }

  {
    aw::PodVector<int> v = {0, 1, 2, 3, 4, 5};
    int *next = v.erase(v.begin() + 1, v.begin() + 4);
    std::vector<int> ref = {0, 1, 2, 3, 4, 5};
    ref.erase(ref.begin() + 1, ref.begin() + 4);
    expectSame(v, ref, "range erase matches std::vector");
    expect(next == v.begin() + 1 && *next == 4, "range erase returns the new position");
  }

  {
    aw::PodVector<int> v = {0, 1, 2};
    v.erase(v.begin() + 1, v.begin() + 1);
    expect(v.size() == 3 && v[1] == 1, "empty range erase is a no-op");

    v.erase(v.begin(), v.end());
    expect(v.empty() && v.data() != nullptr, "full erase empties the vector");
  }

  {
    aw::PodVector<int> v = {10, 20, 30, 40};
    v.erase_unordered(1);
    expect(v.size() == 3 && v[1] == 40 && v[0] == 10 && v[2] == 30,
           "erase_unordered swaps in the last element");
  }

  {
    // std::unique + erase, the idiom the planner relies on.
    aw::PodVector<int> v = {3, 3, 1, 1, 1, 4, 2, 2};
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::vector<int> ref = {1, 2, 3, 4};
    expectSame(v, ref, "sort/unique/erase pipeline matches std::vector");
  }
}

void testAccessAndIteration() {
  std::cout << "[PodVector] access and iteration\n";

  aw::PodVector<int> v = {1, 2, 3};
  const aw::PodVector<int> &c = v;

  expect(v.front() == 1 && v.back() == 3, "front/back");
  expect(c.front() == 1 && c.back() == 3, "const front/back");

  // Non-const access hands out a reference; const access returns by value,
  // matching the design note that integer elements are cheap to pass in a
  // register.
  static_assert(std::is_same_v<decltype(c[0]), int>);
  static_assert(std::is_same_v<decltype(c.front()), int>);
  static_assert(std::is_same_v<decltype(c.back()), int>);
  static_assert(std::is_same_v<decltype(v[0]), int&>);

  v[1] = 20;
  expect(c[1] == 20, "operator[] writes through");
  expect(v.data()[2] == 3, "data() exposes the storage");

  int sum = 0;
  for (int x : v)
    sum += x;
  expect(sum == 24, "range-for iterates every element");

  int csum = 0;
  for (int x : c)
    csum += x;
  expect(csum == 24, "range-for works on a const vector");

  int *it = std::find(v.begin(), v.end(), 20);
  expect(it == v.begin() + 1 && *it == 20, "std::find works on the iterators");

  expect(*std::ranges::max_element(c) == 20, "std::ranges algorithms work");

  const int probe[] = {1, 20, 3};
  expect(std::equal(v.begin(), v.end(), std::begin(probe)), "iterators compare elementwise");

  aw::PodVector<int> sorted = {1, 3, 20};
  expect(std::lower_bound(sorted.begin(), sorted.end(), 3) == sorted.begin() + 1,
         "std::lower_bound works on sorted contents");
}

void testSwapAndShrink() {
  std::cout << "[PodVector] swap / shrink_to_fit\n";

  {
    aw::PodVector<int> a = {1, 2, 3};
    aw::PodVector<int> b = {9, 8};
    a.swap(b);
    std::vector<int> wantA = {9, 8};
    std::vector<int> wantB = {1, 2, 3};
    expectSame(a, wantA, "swap moves the second vector into the first");
    expectSame(b, wantB, "swap moves the first vector into the second");
  }

  {
    aw::PodVector<int> v = {1, 2, 3};
    v.swap(v);
    std::vector<int> want = {1, 2, 3};
    expectSame(v, want, "self swap is a no-op");
  }

  {
    aw::PodVector<int> v;
    for (int i = 0; i < 500; i++)
      v.push_back(i);
    expect(v.capacity() > v.size(), "growth leaves slack");
    v.shrink_to_fit();
    expect(v.capacity() == v.size(), "shrink_to_fit releases the slack");
    for (int i = 0; i < 500; i++)
      expect(v[i] == i, "shrink_to_fit preserves the contents");

    v.shrink_to_fit();
    expect(v.capacity() == v.size(), "shrink_to_fit is idempotent");

    v.clear();
    v.shrink_to_fit();
    expect(v.empty() && v.data() != nullptr, "shrink_to_fit on empty keeps data non-null");
    v.push_back(11);
    expect(v.size() == 1 && v[0] == 11, "the vector is usable after shrinking to empty");
  }
}

void testCopyAndMove() {
  std::cout << "[PodVector] copy and move\n";

  {
    aw::PodVector<int> src = {1, 2, 3};
    aw::PodVector<int> copy(src);
    expectSame(copy, src, "copy construction duplicates the contents");
    copy[0] = 99;
    expect(src[0] == 1, "the copy owns its storage");

    aw::PodVector<int> assigned;
    assigned = src;
    expectSame(assigned, src, "copy assignment duplicates the contents");

    assigned = assigned;
    expectSame(assigned, src, "self copy assignment is a no-op");

    // Assign into a vector with a larger capacity and one with a smaller one.
    aw::PodVector<int> reused(500, 0);
    reused = src;
    expectSame(reused, src, "copy assignment reuses a larger buffer");
    aw::PodVector<int> small(1, 0);
    small = src;
    expectSame(small, src, "copy assignment grows a smaller buffer");
  }

  {
    aw::PodVector<int> src = {4, 5, 6};
    int *storage = src.data();
    aw::PodVector<int> moved(std::move(src));
    expect(moved.data() == storage, "move construction steals the storage");
    expectSame(moved, std::vector<int>{4, 5, 6}, "move construction transfers contents");
    expect(src.empty() && src.data() != nullptr, "the moved-from vector is empty");
    src.push_back(1);
    expect(src.size() == 1 && src[0] == 1, "the moved-from vector is reusable");
  }

  {
    aw::PodVector<int> src = {7, 8};
    aw::PodVector<int> moved;
    moved.push_back(0);
    int *storage = src.data();
    moved = std::move(src);
    expect(moved.data() == storage, "move assignment steals the storage");
    expectSame(moved, std::vector<int>{7, 8}, "move assignment transfers contents");
    expect(src.empty() && src.data() != nullptr, "the move-assigned source is empty");

    // Route the self-move through a pointer so the compiler does not warn
    // about an obviously self-destructive statement; the operator must still
    // guard against it.
    aw::PodVector<int> *alias = &moved;
    moved = std::move(*alias);
    expectSame(moved, std::vector<int>{7, 8}, "self move assignment is a no-op");
  }

  {
    // Elements survive a std::vector<PodVector<...>> round trip, which is how
    // the graph stores nested adjacency lists.
    aw::vector<aw::vector<int>> nested = {{1, 2}, {3}, {}};
    std::vector<aw::PodVector<int>> copy = nested;
    expect(copy.size() == 3, "nested vectors copy");
    expectSame(copy[0], std::vector<int>{1, 2}, "nested element 0 survives");
    expectSame(copy[2], std::vector<int>{}, "nested empty element survives");

    copy.push_back({9, 9, 9});
    copy.pop_back();
    expect(copy.size() == 3, "nested vectors can be pushed and popped");
  }
}

void testEquality() {
  std::cout << "[PodVector] equality\n";

  const aw::PodVector<int> a = {1, 2, 3};
  const aw::PodVector<int> b = {1, 2, 3};
  const aw::PodVector<int> c = {1, 2};
  const aw::PodVector<int> d = {1, 2, 4};

  expect(a == b, "equal contents compare equal");
  expect(!(a != b), "operator!= is derived from operator==");
  expect(a != c, "differing sizes compare unequal");
  expect(a != d, "differing elements compare unequal");

  const aw::PodVector<int> empty1;
  const aw::PodVector<int> empty2;
  expect(empty1 == empty2, "two empty vectors compare equal");

  // std::vector<PodVector<...>> comparison, as used by the pruning tests.
  const std::vector<aw::PodVector<int>> outer = {{1}, {2, 3}};
  const aw::vector<aw::vector<int>> expected = {{1}, {2, 3}};
  expect(outer == expected, "nested comparison uses PodVector::operator==");
}

void testAppendRange() {
  std::cout << "[PodVector] append_range\n";

  aw::PodVector<int> v = {1, 2};
  const int src[] = {3, 4, 5};
  v.append_range(src, 3);
  std::vector<int> ref = {1, 2, 3, 4, 5};
  expectSame(v, ref, "append_range matches std::vector");

  v.append_range(src, 0);
  expectSame(v, ref, "append_range with count 0 is a no-op");

  aw::PodVector<int> big;
  std::vector<int> bigRef;
  const int wide[200] = {};
  big.append_range(wide, 200);
  for (int x : wide)
    bigRef.push_back(x);
  expectSame(big, bigRef, "append_range across a reallocation");
}

void testDifferentialAgainstStdVector() {
  std::cout << "[PodVector] randomized differential test\n";

  aw::PodVector<int> v;
  std::vector<int> ref;
  Rng rng;

  for (int step = 0; step < 20000; step++) {
    switch (rng.below(10)) {
      case 0:
      case 1: {
        const int value = rng.value();
        v.push_back(value);
        ref.push_back(value);
        break;
      }
      case 2: {
        if (!ref.empty()) {
          v.pop_back();
          ref.pop_back();
        }
        break;
      }
      case 3: {
        const size_t pos = rng.below((uint32_t) ref.size() + 1);
        const int value = rng.value();
        v.insert(v.begin() + pos, value);
        ref.insert(ref.begin() + pos, value);
        break;
      }
      case 4: {
        if (!ref.empty()) {
          const size_t pos = rng.below((uint32_t) ref.size());
          v.erase(v.begin() + pos);
          ref.erase(ref.begin() + pos);
        }
        break;
      }
      case 5: {
        if (!ref.empty()) {
          const size_t from = rng.below((uint32_t) ref.size());
          const size_t to = from + rng.below((uint32_t) (ref.size() - from) + 1);
          v.erase(v.begin() + from, v.begin() + to);
          ref.erase(ref.begin() + from, ref.begin() + to);
        }
        break;
      }
      case 6: {
        const size_t count = rng.below(80);
        const int value = rng.value();
        v.resize(count, value);
        ref.resize(count, value);
        break;
      }
      case 7: {
        const size_t count = rng.below(80);
        const int value = rng.value();
        v.assign(count, value);
        ref.assign(count, value);
        break;
      }
      case 8: {
        if (!ref.empty()) {
          const size_t pos = rng.below((uint32_t) ref.size() + 1);
          const size_t count = rng.below(6);
          const int value = rng.value();
          v.insert(v.begin() + pos, count, value);
          ref.insert(ref.begin() + pos, count, value);
        }
        break;
      }
      default: {
        v.clear();
        ref.clear();
        break;
      }
    }

    if (!same(v, ref)) {
      expect(false, "randomized differential test");
      std::cout << "    diverged at step " << step << " (size " << v.size() << " vs "
                << ref.size() << ")\n";
      return;
    }
    expect(v.size() == ref.size(), "randomized differential test");
  }

  expect(v.capacity() >= v.size(), "randomized differential test: capacity invariant");
  expect(v.data() != nullptr, "randomized differential test: data non-null");
}

void testLargeValues() {
  std::cout << "[PodVector] element types\n";

  {
    aw::PodVector<int64_t> v;
    std::vector<int64_t> ref;
    const int64_t probes[] = {0, 1, -1, std::numeric_limits<int64_t>::max(),
                              std::numeric_limits<int64_t>::min()};
    for (int64_t x : probes) {
      v.push_back(x);
      ref.push_back(x);
    }
    v.insert(v.begin() + 2, std::numeric_limits<int64_t>::min());
    ref.insert(ref.begin() + 2, std::numeric_limits<int64_t>::min());
    expectSame(v, ref, "64-bit elements round-trip");
  }

  {
    aw::PodVector<uint8_t> v;
    std::vector<uint8_t> ref;
    for (int i = 0; i < 300; i++) {
      v.push_back((uint8_t) (i & 0xFF));
      ref.push_back((uint8_t) (i & 0xFF));
    }
    expectSame(v, ref, "byte elements round-trip");
  }
}

}  // namespace

int main() {
  testConstruction();
  testAlias();
  testPushPop();
  testReserveResize();
  testAssign();
  testInsert();
  testErase();
  testAccessAndIteration();
  testSwapAndShrink();
  testCopyAndMove();
  testEquality();
  testAppendRange();
  testDifferentialAgainstStdVector();
  testLargeValues();

  if (failures == 0) {
    std::cout << "all PodVector tests passed\n";
    return 0;
  }
  std::cout << failures << " check(s) failed\n";
  return 1;
}
